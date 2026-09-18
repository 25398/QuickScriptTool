#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ext_bridge_server.h"

#include <windows.h>
#include <bcrypt.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace windowmode {
namespace {

std::wstring ModuleDir() {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return {};
    std::wstring full(path);
    const auto slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return full.substr(0, slash);
}

std::wstring ExePath() {
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return {};
    return path;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
        static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
        out.data(), n, nullptr, nullptr);
    return out;
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out.push_back(static_cast<char>(c)); break;
        }
    }
    return out;
}

bool Sha256(const void* data, size_t len, unsigned char out[32]) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objLen = 0, cb = sizeof(objLen);
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        return false;
    if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objLen),
            sizeof(objLen), &cb, 0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return false;
    }
    std::vector<BYTE> obj(objLen);
    bool ok = false;
    if (BCryptCreateHash(alg, &hash, obj.data(), objLen, nullptr, 0, 0) == 0
        && BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<void*>(data)),
            static_cast<ULONG>(len), 0) == 0
        && BCryptFinishHash(hash, out, 32, 0) == 0) {
        ok = true;
    }
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

std::string CrxIdFromUtf8(const std::string& input) {
    unsigned char hash[32]{};
    if (!Sha256(input.data(), input.size(), hash)) return {};
    std::string id(32, 'a');
    for (int i = 0; i < 16; ++i) {
        id[static_cast<size_t>(i) * 2] =
            static_cast<char>('a' + ((hash[i] >> 4) & 0xF));
        id[static_cast<size_t>(i) * 2 + 1] =
            static_cast<char>('a' + (hash[i] & 0xF));
    }
    return id;
}

bool LooksLikeExtId(const std::string& s) {
    if (s.size() != 32) return false;
    for (char c : s) {
        if (c < 'a' || c > 'p') return false;
    }
    return true;
}

void AddId(std::vector<std::string>& ids, const std::string& id) {
    if (!LooksLikeExtId(id)) return;
    if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
}

void AddUnpackedIdFromPath(std::vector<std::string>& ids, std::wstring dir) {
    if (dir.empty()) return;
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    if (dir.size() >= 2 && dir[1] == L':') {
        if (dir[0] >= L'a' && dir[0] <= L'z')
            dir[0] = static_cast<wchar_t>(dir[0] - L'a' + L'A');
    }
    AddId(ids, CrxIdFromUtf8(WideToUtf8(dir)));
}

std::string ReadAllUtf8(const std::wstring& path, size_t maxBytes = 8 * 1024 * 1024) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) return {};
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (s.size() > maxBytes) s.resize(maxBytes);
    return s;
}

bool ManifestLooksLikeOurs(const std::string& json) {
    return json.find("\xe9\x94\xae\xe9\xbc\xa0\xe5\xb7\xa5\xe5\x9d\x8a") != std::string::npos
        || json.find("QuickScriptTool") != std::string::npos
        || json.find("QstEdgeBridge") != std::string::npos;
}

void CollectIdsFromPreferences(const std::string& json, std::vector<std::string>& ids) {
    if (json.empty()) return;
    const std::string needle = "\xe9\x94\xae\xe9\xbc\xa0\xe5\xb7\xa5\xe5\x9d\x8a"; // 键鼠工坊
    size_t pos = 0;
    while ((pos = json.find(needle, pos)) != std::string::npos) {
        const size_t lo = pos > 4000 ? pos - 4000 : 0;
        for (size_t i = pos; i > lo + 34; --i) {
            if (json[i] != '"') continue;
            if (i >= 33 && json[i - 33] == '"') {
                const std::string cand = json.substr(i - 32, 32);
                if (LooksLikeExtId(cand)) {
                    AddId(ids, cand);
                    break;
                }
            }
        }
        pos += needle.size();
    }
    const char* pathNeedles[] = {
        "extension\\\\edge", "extension/edge", "extension\\edge"
    };
    for (const char* pn : pathNeedles) {
        size_t p = 0;
        while ((p = json.find(pn, p)) != std::string::npos) {
            const size_t lo = p > 4000 ? p - 4000 : 0;
            for (size_t i = p; i > lo + 34; --i) {
                if (json[i] != '"') continue;
                if (i >= 33 && json[i - 33] == '"') {
                    const std::string cand = json.substr(i - 32, 32);
                    if (LooksLikeExtId(cand)) {
                        AddId(ids, cand);
                        break;
                    }
                }
            }
            p += 8;
        }
    }
}

void ScanExtensionsTree(const std::wstring& extRoot, std::vector<std::string>& ids) {
    WIN32_FIND_DATAW fd{};
    const std::wstring glob = extRoot + L"\\*";
    HANDLE h = FindFirstFileW(glob.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        const std::string id = WideToUtf8(fd.cFileName);
        if (!LooksLikeExtId(id)) continue;
        const std::wstring idDir = extRoot + L"\\" + fd.cFileName;
        WIN32_FIND_DATAW vd{};
        HANDLE hv = FindFirstFileW((idDir + L"\\*").c_str(), &vd);
        if (hv == INVALID_HANDLE_VALUE) continue;
        do {
            if (!(vd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (vd.cFileName[0] == L'.') continue;
            const std::wstring man = idDir + L"\\" + vd.cFileName + L"\\manifest.json";
            const std::string body = ReadAllUtf8(man, 64 * 1024);
            if (ManifestLooksLikeOurs(body)) AddId(ids, id);
        } while (FindNextFileW(hv, &vd));
        FindClose(hv);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void ScanBrowserUserData(const std::wstring& userData, std::vector<std::string>& ids) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((userData + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        const std::wstring profile = userData + L"\\" + fd.cFileName;
        CollectIdsFromPreferences(ReadAllUtf8(profile + L"\\Preferences"), ids);
        CollectIdsFromPreferences(ReadAllUtf8(profile + L"\\Secure Preferences"), ids);
        ScanExtensionsTree(profile + L"\\Extensions", ids);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::wstring LocalApp(const wchar_t* extra) {
    wchar_t dir[MAX_PATH]{};
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH) == 0) return {};
    return std::wstring(dir) + extra;
}

std::vector<std::string> DiscoverExtensionIds() {
    std::vector<std::string> ids;
    AddUnpackedIdFromPath(ids, ExtensionEdgeDirectory());
    std::wstring dir = ModuleDir();
    for (int i = 0; i < 6; ++i) {
        const std::wstring cand = dir + L"\\extension\\edge";
        if (GetFileAttributesW((cand + L"\\background.js").c_str()) != INVALID_FILE_ATTRIBUTES)
            AddUnpackedIdFromPath(ids, cand);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) break;
        dir = dir.substr(0, slash);
    }
    const std::wstring local = LocalApp(L"");
    if (!local.empty()) {
        ScanBrowserUserData(local + L"\\Microsoft\\Edge\\User Data", ids);
        ScanBrowserUserData(local + L"\\Google\\Chrome\\User Data", ids);
        ScanBrowserUserData(local + L"\\Microsoft\\Edge Beta\\User Data", ids);
        ScanBrowserUserData(local + L"\\Chromium\\User Data", ids);
    }
    return ids;
}

bool WriteUtf8File(const std::wstring& path, const std::string& body) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = WriteFile(h, body.data(), static_cast<DWORD>(body.size()),
        &written, nullptr);
    CloseHandle(h);
    return ok != FALSE;
}

void SetHkcuNativeHost(const wchar_t* browserKey, const std::wstring& manifestPath) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, browserKey, 0, nullptr, 0, KEY_SET_VALUE,
            nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return;
    }
    const DWORD bytes = static_cast<DWORD>((manifestPath.size() + 1) * sizeof(wchar_t));
    RegSetValueExW(key, nullptr, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(manifestPath.c_str()), bytes);
    RegCloseKey(key);
}

bool ReadExact(HANDLE h, void* buf, DWORD n) {
    auto* p = static_cast<unsigned char*>(buf);
    DWORD got = 0;
    while (got < n) {
        DWORD r = 0;
        if (!ReadFile(h, p + got, n - got, &r, nullptr) || r == 0) return false;
        got += r;
    }
    return true;
}

bool WriteExact(HANDLE h, const void* buf, DWORD n) {
    const auto* p = static_cast<const unsigned char*>(buf);
    DWORD got = 0;
    while (got < n) {
        DWORD w = 0;
        if (!WriteFile(h, p + got, n - got, &w, nullptr) || w == 0) return false;
        got += w;
    }
    return true;
}

std::string ReadBridgeFileJson() {
    const std::wstring path = LocalApp(L"\\QuickScriptTool\\ext_bridge.json");
    if (path.empty()) return "{\"ok\":false,\"error\":\"no_appdata\"}";
    const std::string body = ReadAllUtf8(path, 4096);
    if (body.find("\"token\"") == std::string::npos) {
        return "{\"ok\":false,\"error\":\"no_bridge\"}";
    }
    return body;
}

}  // namespace

void RegisterExtNativeMessagingHost() {
    const std::wstring exe = ExePath();
    const std::wstring dir = ModuleDir();
    if (exe.empty() || dir.empty()) return;
    const auto ids = DiscoverExtensionIds();
    std::string json = "{\n  \"name\": \"com.quickscripttool.bridge\",\n"
        "  \"description\": \"\xe9\x94\xae\xe9\xbc\xa0\xe5\xb7\xa5\xe5\x9d\x8a\xe6\xa1\xa5\",\n"
        "  \"path\": \"" + JsonEscape(WideToUtf8(exe)) + "\",\n"
        "  \"type\": \"stdio\",\n"
        "  \"allowed_origins\": [";
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) json += ", ";
        json += "\"chrome-extension://";
        json += ids[i];
        json += "/\"";
    }
    json += "]\n}\n";
    const std::wstring manifest = dir + L"\\com.quickscripttool.bridge.json";
    WriteUtf8File(manifest, json);
    SetHkcuNativeHost(
        L"Software\\Microsoft\\Edge\\NativeMessagingHosts\\com.quickscripttool.bridge",
        manifest);
    SetHkcuNativeHost(
        L"Software\\Google\\Chrome\\NativeMessagingHosts\\com.quickscripttool.bridge",
        manifest);
    SetHkcuNativeHost(
        L"Software\\Microsoft\\Edge Beta\\NativeMessagingHosts\\com.quickscripttool.bridge",
        manifest);
}

int RunExtNativeMessagingHost() {
    RegisterExtNativeMessagingHost();
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!in || in == INVALID_HANDLE_VALUE || !out || out == INVALID_HANDLE_VALUE)
        return 1;
    for (;;) {
        uint32_t n = 0;
        if (!ReadExact(in, &n, 4)) break;
        if (n == 0 || n > 1024 * 1024) break;
        std::string req(n, '\0');
        if (!ReadExact(in, req.data(), n)) break;
        const std::string reply = ReadBridgeFileJson();
        const uint32_t m = static_cast<uint32_t>(reply.size());
        if (!WriteExact(out, &m, 4)) break;
        if (!WriteExact(out, reply.data(), m)) break;
        FlushFileBuffers(out);
    }
    return 0;
}

}  // namespace windowmode
