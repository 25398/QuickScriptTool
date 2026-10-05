#include "ooxml/zip_archive.h"

#include "ooxml/inflate.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace qst {
namespace ooxml {

namespace {

#pragma pack(push, 1)
struct LocalHeader {
    uint32_t signature;      // 0x04034B50
    uint16_t versionNeeded;
    uint16_t flags;
    uint16_t method;
    uint16_t modTime;
    uint16_t modDate;
    uint32_t crc32;
    uint32_t compSize;
    uint32_t uncompSize;
    uint16_t nameLen;
    uint16_t extraLen;
};
struct CentralHeader {
    uint32_t signature;      // 0x02014B50
    uint16_t versionMadeBy;
    uint16_t versionNeeded;
    uint16_t flags;
    uint16_t method;
    uint16_t modTime;
    uint16_t modDate;
    uint32_t crc32;
    uint32_t compSize;
    uint32_t uncompSize;
    uint16_t nameLen;
    uint16_t extraLen;
    uint16_t commentLen;
    uint16_t diskStart;
    uint16_t internalAttr;
    uint32_t externalAttr;
    uint32_t localOffset;
};
struct Eocd {
    uint32_t signature;      // 0x06054B50
    uint16_t diskNum;
    uint16_t cdDisk;
    uint16_t entriesOnDisk;
    uint16_t totalEntries;
    uint32_t cdSize;
    uint32_t cdOffset;
    uint16_t commentLen;
};
struct Zip64EocdLocator {
    uint32_t signature;      // 0x07064B50
    uint32_t diskWithZip64Eocd;
    uint64_t zip64EocdOffset;
    uint32_t totalDisks;
};
#pragma pack(pop)

constexpr uint32_t kSigLocal = 0x04034B50;
constexpr uint32_t kSigCentral = 0x02014B50;
constexpr uint32_t kSigEocd = 0x06054B50;
constexpr uint32_t kSigZip64Locator = 0x07064B50;

constexpr uint16_t kMethodStored = 0;
constexpr uint16_t kMethodDeflate = 8;

bool ReadU32(const std::vector<uint8_t>& b, size_t off, uint32_t& v) {
    if (off + 4 > b.size()) return false;
    std::memcpy(&v, b.data() + off, 4);
    return true;
}

/// 从尾部找 EOCD（注释最长 65535，所以最多回看 22 + 65535 字节）
bool FindEocd(const std::vector<uint8_t>& b, Eocd& out, size_t& outOff) {
    if (b.size() < sizeof(Eocd)) return false;
    const size_t start = (b.size() > 22 + 65535) ? (b.size() - 22 - 65535) : 0;
    for (size_t pos = b.size() - sizeof(Eocd) + 1; pos-- > start;) {
        uint32_t sig = 0;
        if (!ReadU32(b, pos, sig)) continue;
        if (sig != kSigEocd) continue;
        std::memcpy(&out, b.data() + pos, sizeof(out));
        // 注释长度必须与剩余字节吻合，否则是数据里偶然出现的假签名
        if (pos + sizeof(Eocd) + out.commentLen != b.size()) continue;
        outOff = pos;
        return true;
    }
    return false;
}

void PushU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
}
void PushU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(static_cast<uint8_t>(x & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 8) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 16) & 0xFF));
    v.push_back(static_cast<uint8_t>((x >> 24) & 0xFF));
}
void PushBytes(std::vector<uint8_t>& v, const void* p, size_t n) {
    const auto* c = static_cast<const uint8_t*>(p);
    v.insert(v.end(), c, c + n);
}

}  // namespace

bool ReadZipFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
#ifdef _WIN32
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
    DWORD got = 0;
    const BOOL ok = out.empty()
        || (ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr)
            && got == out.size());
    CloseHandle(h);
    return ok != 0;
#else
    (void)path; (void)out;
    return false;
#endif
}

bool WriteZipFileBytes(const std::wstring& path, const std::vector<uint8_t>& data) {
#ifdef _WIN32
    const HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t written = 0;
    bool ok = true;
    while (written < data.size()) {
        DWORD chunk = static_cast<DWORD>((data.size() - written > 0x7FFFFFFF)
            ? 0x7FFFFFFF : (data.size() - written));
        DWORD n = 0;
        if (!WriteFile(h, data.data() + written, chunk, &n, nullptr) || n == 0) { ok = false; break; }
        written += n;
    }
    CloseHandle(h);
    return ok;
#else
    (void)path; (void)data;
    return false;
#endif
}

uint32_t ZipCrc32(const void* data, size_t len) {
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

bool ZipArchive::LoadFromMemory(const uint8_t* data, size_t size, std::string& err) {
    entries_.clear();
    err.clear();
    if (data == nullptr || size < sizeof(Eocd)) {
        err = "zip 数据太短";
        return false;
    }
    std::vector<uint8_t> buf(data, data + size);

    Eocd eocd{};
    size_t eocdOff = 0;
    if (!FindEocd(buf, eocd, eocdOff)) {
        err = "找不到 zip 的 EOCD（不是合法 zip？）";
        return false;
    }
    // Zip64：EOCD 里的计数字段会是 0xFFFF/0xFFFFFFFF，紧邻前面有 Zip64 locator。
    // 与其按 32 位错读出一堆垃圾，不如**明确拒绝**。
    if (eocd.totalEntries == 0xFFFF || eocd.cdOffset == 0xFFFFFFFFu
        || eocd.cdSize == 0xFFFFFFFFu) {
        err = "该 zip 使用 Zip64（>4GB 或 >65535 条目），当前不支持";
        return false;
    }
    if (eocdOff >= sizeof(Zip64EocdLocator)) {
        uint32_t sig = 0;
        if (ReadU32(buf, eocdOff - sizeof(Zip64EocdLocator), sig) && sig == kSigZip64Locator) {
            err = "该 zip 使用 Zip64，当前不支持";
            return false;
        }
    }
    if (eocd.cdOffset + eocd.cdSize > buf.size()) {
        err = "zip 中央目录越界";
        return false;
    }

    size_t pos = eocd.cdOffset;
    for (uint32_t i = 0; i < eocd.totalEntries; ++i) {
        if (pos + sizeof(CentralHeader) > buf.size()) { err = "中央目录条目越界"; return false; }
        CentralHeader ch{};
        std::memcpy(&ch, buf.data() + pos, sizeof(ch));
        pos += sizeof(ch);
        if (ch.signature != kSigCentral) { err = "中央目录签名不对"; return false; }
        if (pos + ch.nameLen + ch.extraLen + ch.commentLen > buf.size()) {
            err = "中央目录条目名越界";
            return false;
        }
        ZipEntry e;
        e.name.assign(reinterpret_cast<const char*>(buf.data() + pos), ch.nameLen);
        pos += ch.nameLen + ch.extraLen + ch.commentLen;
        e.method = ch.method;
        e.crc32 = ch.crc32;
        e.compSize = ch.compSize;
        e.uncompSize = ch.uncompSize;

        // 从本地头拿到数据真正开始的位置（本地头的 name/extra 长度可能与中央目录不同！）
        if (static_cast<uint64_t>(ch.localOffset) + sizeof(LocalHeader) > buf.size()) {
            err = "本地头越界：" + e.name;
            return false;
        }
        LocalHeader lh{};
        std::memcpy(&lh, buf.data() + ch.localOffset, sizeof(lh));
        if (lh.signature != kSigLocal) { err = "本地头签名不对：" + e.name; return false; }
        const size_t dataOff = static_cast<size_t>(ch.localOffset) + sizeof(LocalHeader)
            + lh.nameLen + lh.extraLen;
        if (dataOff + ch.compSize > buf.size()) { err = "条目数据越界：" + e.name; return false; }
        e.rawComp.assign(buf.begin() + dataOff, buf.begin() + dataOff + ch.compSize);

        if (e.method != kMethodStored && e.method != kMethodDeflate) {
            err = "不支持的压缩方式（method=" + std::to_string(e.method) + "）：" + e.name;
            return false;
        }
        entries_.push_back(std::move(e));
    }
    return true;
}

bool ZipArchive::LoadFromFile(const std::wstring& path, std::string& err) {
    std::vector<uint8_t> bytes;
    if (!ReadZipFileBytes(path, bytes)) {
        err = "读文件失败";
        return false;
    }
    return LoadFromMemory(bytes.data(), bytes.size(), err);
}

const ZipEntry* ZipArchive::Find(const std::string& name) const {
    for (const auto& e : entries_) {
        if (e.name == name) return &e;
    }
    return nullptr;
}

bool ZipArchive::GetData(const std::string& name, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    err.clear();
    for (auto& e : entries_) {
        if (e.name != name) continue;
        if (e.dataLoaded) { out = e.data; return true; }
        if (e.method == kMethodStored) {
            e.data = e.rawComp;
        } else {
            if (!InflateRaw(e.rawComp.data(), e.rawComp.size(),
                    static_cast<size_t>(e.uncompSize), e.data, err)) {
                err = "解压失败（" + name + "）：" + err;
                return false;
            }
        }
        e.dataLoaded = true;
        out = e.data;
        return true;
    }
    err = "没有这个条目：" + name;
    return false;
}

bool ZipArchive::GetText(const std::string& name, std::string& out, std::string& err) {
    std::vector<uint8_t> bytes;
    if (!GetData(name, bytes, err)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

bool ZipArchive::SetData(const std::string& name, std::vector<uint8_t> data, std::string& err) {
    err.clear();
    for (auto& e : entries_) {
        if (e.name != name) continue;
        e.data = std::move(data);
        e.dataLoaded = true;
        e.modified = true;
        return true;
    }
    err = "没有这个条目：" + name;
    return false;
}

bool ZipArchive::AddEntry(const std::string& name, std::vector<uint8_t> data, std::string& err) {
    err.clear();
    if (name.empty()) { err = "条目名不能为空"; return false; }
    if (Find(name) != nullptr) { err = "条目已存在：" + name; return false; }
    ZipEntry e;
    e.name = name;
    e.method = kMethodStored;
    e.data = std::move(data);
    e.dataLoaded = true;
    e.modified = true;
    entries_.push_back(std::move(e));
    return true;
}

bool ZipArchive::RemoveEntry(const std::string& name) {
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].name == name) {
            entries_.erase(entries_.begin() + static_cast<long>(i));
            return true;
        }
    }
    return false;
}

bool ZipArchive::SaveToMemory(std::vector<uint8_t>& out, std::string& err) const {
    out.clear();
    err.clear();
    if (entries_.empty()) { err = "没有条目可写"; return false; }

    struct Written {
        const ZipEntry* e;
        uint32_t crc = 0;
        uint32_t compSize = 0;
        uint32_t uncompSize = 0;
        uint16_t method = 0;
        uint32_t offset = 0;
        const std::vector<uint8_t>* payload = nullptr;   // 要写的字节（压缩后）
    };
    std::vector<Written> done;
    done.reserve(entries_.size());

    for (const auto& e : entries_) {
        Written w;
        w.e = &e;
        // ⚠ A/B 验证过：把这里改成「无条件重编码」⇒ OoxmlSelfTest 的
        //   `zip_byte_preserving_save` / `zip_preserves_method_and_crc` 双双转红
        //   （未改动条目没加载内容，连写都写不出来）⇒ 用例不是空断言。
        if (e.modified) {
            // 改过 / 新增 ⇒ 以 stored 重新编码（Excel/Word 完全接受）
            if (!e.dataLoaded) { err = "条目内容未加载，无法写入：" + e.name; return false; }
            w.method = kMethodStored;
            w.uncompSize = static_cast<uint32_t>(e.data.size());
            w.compSize = w.uncompSize;
            w.crc = ZipCrc32(e.data.data(), e.data.size());
            w.payload = &e.data;
        } else {
            // ★ 未改动 ⇒ **原始压缩字节 + 原始 method/crc 原样搬运**（字节保留的关键）
            w.method = e.method;
            w.compSize = static_cast<uint32_t>(e.compSize);
            w.uncompSize = static_cast<uint32_t>(e.uncompSize);
            w.crc = e.crc32;
            w.payload = &e.rawComp;
        }
        if (e.name.size() > 0xFFFF) { err = "条目名过长：" + e.name; return false; }
        done.push_back(w);
    }

    for (auto& w : done) {
        if (out.size() > 0xFFFFFFFFu) { err = "zip 超过 4GB，当前不支持"; return false; }
        w.offset = static_cast<uint32_t>(out.size());
        LocalHeader lh{};
        lh.signature = kSigLocal;
        lh.versionNeeded = 20;
        lh.flags = 0;                 // 我们自带真实 crc/size ⇒ 不设 data descriptor 位
        lh.method = w.method;
        lh.modTime = 0;
        lh.modDate = 0x21;            // 1980-01-01（合法的最小 DOS 日期）
        lh.crc32 = w.crc;
        lh.compSize = w.compSize;
        lh.uncompSize = w.uncompSize;
        lh.nameLen = static_cast<uint16_t>(w.e->name.size());
        lh.extraLen = 0;
        PushBytes(out, &lh, sizeof(lh));
        PushBytes(out, w.e->name.data(), w.e->name.size());
        PushBytes(out, w.payload->data(), w.payload->size());
    }

    const size_t cdOffset = out.size();
    for (const auto& w : done) {
        CentralHeader ch{};
        ch.signature = kSigCentral;
        ch.versionMadeBy = 20;
        ch.versionNeeded = 20;
        ch.flags = 0;
        ch.method = w.method;
        ch.modDate = 0x21;
        ch.crc32 = w.crc;
        ch.compSize = w.compSize;
        ch.uncompSize = w.uncompSize;
        ch.nameLen = static_cast<uint16_t>(w.e->name.size());
        ch.localOffset = w.offset;
        PushBytes(out, &ch, sizeof(ch));
        PushBytes(out, w.e->name.data(), w.e->name.size());
    }
    const size_t cdSize = out.size() - cdOffset;
    if (out.size() > 0xFFFFFFFFu || done.size() > 0xFFFF) {
        err = "zip 超过 4GB / 65535 条目，当前不支持";
        return false;
    }

    Eocd eocd{};
    eocd.signature = kSigEocd;
    eocd.entriesOnDisk = static_cast<uint16_t>(done.size());
    eocd.totalEntries = static_cast<uint16_t>(done.size());
    eocd.cdSize = static_cast<uint32_t>(cdSize);
    eocd.cdOffset = static_cast<uint32_t>(cdOffset);
    PushBytes(out, &eocd, sizeof(eocd));
    return true;
}

bool ZipArchive::SaveToFile(const std::wstring& path, std::string& err) const {
    std::vector<uint8_t> bytes;
    if (!SaveToMemory(bytes, err)) return false;
    if (!WriteZipFileBytes(path, bytes)) {
        err = "写文件失败";
        return false;
    }
    return true;
}

}  // namespace ooxml
}  // namespace qst
