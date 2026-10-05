// ──────────────────────────────────────────────────────────────────
// agent_mcp.cpp — MCP 客户端实现（设计说明见头文件）
// ──────────────────────────────────────────────────────────────────

#include "agent_mcp.h"

#include "base64.h"
#include "utils.h"

#include <nlohmann/json.hpp>

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cwctype>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

using json = nlohmann::json;

namespace qst {
namespace agent {
namespace {

/// 我们声明的协议版本。服务端会在 initialize 结果里回它支持的版本，
/// 我们**不按版本分叉**（只用 tools/list 与 tools/call，这两个在已知版本里语义一致）。
/// 若哪天要用版本相关特性，必须在这里做协商，不能假设服务端认我们的版本。
const char* const kProtocolVersion = "2024-11-05";

/// 消息里绝不能出现裸换行 —— stdio 传输是按行分帧的。
/// 这里做最后一道保险：上游万一拼了原始文本进来，也不会把流拆坏。
std::string StripNewlines(std::string s) {
    for (char& c : s) {
        if (c == '\n' || c == '\r') c = ' ';
    }
    return s;
}

std::wstring Utf8ToW(const std::string& s) { return FromUtf8(s); }
std::string WToUtf8(const std::wstring& s) { return ToUtf8(s); }

std::wstring LowerCopy(const std::wstring& s) {
    std::wstring out = s;
    std::transform(out.begin(), out.end(), out.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return out;
}

std::string TrimAscii(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'
        || s.back() == '\n')) {
        s.pop_back();
    }
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

/// CreateProcess 的命令行引号规则：含空格/制表/引号的参数必须整体加引号，
/// 且内部的 `"` 要转义成 `\"`（反斜杠要成对加倍）。
std::wstring QuoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out.push_back(c);
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::wstring BuildCommandLine(const McpServerConfig& cfg) {
    std::wstring cmd = QuoteArg(cfg.command);
    for (const auto& a : cfg.args) {
        cmd.push_back(L' ');
        cmd += QuoteArg(a);
    }
    return cmd;
}

}  // namespace

// ── 配置 ─────────────────────────────────────────────────────────

std::wstring McpServersConfigPath() {
    return AppDir() + L"\\mcp_servers.json";
}

bool ParseMcpServersJson(const std::string& jsonText,
    std::vector<McpServerConfig>& out, std::wstring& err) {
    out.clear();
    err.clear();
    const std::string text = TrimAscii(jsonText);
    // 空文件 = 没有外部 server，不是错误
    if (text.empty()) return true;
    json j;
    try {
        j = json::parse(text);
    } catch (const json::parse_error&) {
        err = L"mcp_servers.json 不是合法 JSON";
        return false;
    }
    if (!j.is_object() || !j.contains("servers") || !j["servers"].is_array()) {
        err = L"mcp_servers.json 需要形如 {\"servers\":[…] }";
        return false;
    }
    for (const auto& item : j["servers"]) {
        if (!item.is_object()) continue;
        McpServerConfig cfg;
        if (item.contains("name") && item["name"].is_string())
            cfg.name = FromUtf8(item["name"].get<std::string>());
        if (item.contains("command") && item["command"].is_string())
            cfg.command = FromUtf8(item["command"].get<std::string>());
        if (item.contains("cwd") && item["cwd"].is_string())
            cfg.cwd = FromUtf8(item["cwd"].get<std::string>());
        if (item.contains("args") && item["args"].is_array()) {
            for (const auto& a : item["args"]) {
                if (a.is_string()) cfg.args.push_back(FromUtf8(a.get<std::string>()));
            }
        }
        if (item.contains("enabled") && item["enabled"].is_boolean())
            cfg.enabled = item["enabled"].get<bool>();
        // 缺 name 或 command 的条目直接跳过：起不来只会变成一条每次调用都失败的
        // 工具，白白污染工具表、还让模型以为「这个能力存在但坏了」。
        if (cfg.name.empty() || cfg.command.empty()) continue;
        out.push_back(std::move(cfg));
    }
    return true;
}

std::string SerializeMcpServersJson(const std::vector<McpServerConfig>& servers) {
    json root;
    root["servers"] = json::array();
    for (const auto& s : servers) {
        json item;
        item["name"] = WToUtf8(s.name);
        item["command"] = WToUtf8(s.command);
        json args = json::array();
        for (const auto& a : s.args) args.push_back(WToUtf8(a));
        item["args"] = args;
        if (!s.cwd.empty()) item["cwd"] = WToUtf8(s.cwd);
        item["enabled"] = s.enabled;
        root["servers"].push_back(item);
    }
    return root.dump(2);
}

bool LoadMcpServers(std::vector<McpServerConfig>& out, std::wstring& err) {
    out.clear();
    err.clear();
    std::ifstream f(McpServersConfigPath(), std::ios::binary);
    if (!f) return true;   // 文件不存在 = 没有外部 server
    std::ostringstream oss;
    oss << f.rdbuf();
    return ParseMcpServersJson(oss.str(), out, err);
}

// ── 协议层 ───────────────────────────────────────────────────────

std::string McpBuildRequestLine(int id, const std::string& method,
    const std::string& paramsJson) {
    json req;
    req["jsonrpc"] = "2.0";
    req["id"] = id;
    req["method"] = method;
    if (!paramsJson.empty()) {
        json params;
        try {
            params = json::parse(paramsJson);
        } catch (const json::parse_error&) {
            // 上游给了坏 params：宁可发一个空对象（服务端会报参数错，我们能看见），
            // 也不要把半截文本拼进消息里把整条流拆坏。
            params = json::object();
        }
        req["params"] = params;
    }
    return StripNewlines(req.dump());
}

std::string McpBuildNotificationLine(const std::string& method,
    const std::string& paramsJson) {
    json req;
    req["jsonrpc"] = "2.0";
    req["method"] = method;
    if (!paramsJson.empty()) {
        json params;
        try {
            params = json::parse(paramsJson);
        } catch (const json::parse_error&) {
            params = json::object();
        }
        req["params"] = params;
    }
    return StripNewlines(req.dump());
}

McpLineKind McpParseResponseLine(const std::string& line, int id,
    std::string& resultJson, std::wstring& errOut) {
    resultJson.clear();
    errOut.clear();
    const std::string text = TrimAscii(line);
    if (text.empty()) return McpLineKind::Ignore;
    json j;
    try {
        j = json::parse(text);
    } catch (const json::parse_error&) {
        // 服务端往 stdout 打了一行日志：**不是错误**，忽略即可。
        // （把日志当错误会让「能用的 server」被判成坏的 —— 实测很常见。）
        return McpLineKind::Ignore;
    }
    if (!j.is_object()) return McpLineKind::Ignore;
    if (!j.contains("id") || !j["id"].is_number_integer()) return McpLineKind::Ignore;
    if (j["id"].get<int>() != id) return McpLineKind::Ignore;
    if (j.contains("error")) {
        const json& e = j["error"];
        std::wstring msg = L"MCP 服务端返回错误";
        if (e.is_object() && e.contains("message") && e["message"].is_string())
            msg += L"：" + FromUtf8(e["message"].get<std::string>());
        errOut = msg;
        return McpLineKind::Error;
    }
    if (!j.contains("result")) {
        errOut = L"MCP 响应缺少 result 字段";
        return McpLineKind::Error;
    }
    resultJson = j["result"].dump();
    return McpLineKind::Result;
}

bool McpParseToolsList(const std::string& resultJson,
    std::vector<McpRemoteTool>& out, std::wstring& err) {
    out.clear();
    err.clear();
    json j;
    try {
        j = json::parse(resultJson);
    } catch (const json::parse_error&) {
        err = L"tools/list 结果不是合法 JSON";
        return false;
    }
    if (!j.is_object() || !j.contains("tools") || !j["tools"].is_array()) {
        err = L"tools/list 结果缺少 tools 数组";
        return false;
    }
    for (const auto& t : j["tools"]) {
        if (!t.is_object()) continue;
        McpRemoteTool rt;
        if (t.contains("name") && t["name"].is_string())
            rt.name = t["name"].get<std::string>();
        if (rt.name.empty()) continue;
        if (t.contains("description") && t["description"].is_string())
            rt.description = t["description"].get<std::string>();
        if (t.contains("inputSchema")) rt.inputSchema = t["inputSchema"].dump();
        out.push_back(std::move(rt));
    }
    return true;
}

std::string McpQualifiedToolName(const std::wstring& serverName,
    const std::string& toolName) {
    return "mcp__" + WToUtf8(serverName) + "__" + toolName;
}

bool McpSplitQualifiedToolName(const std::string& qualified,
    std::wstring& server, std::string& tool) {
    server.clear();
    tool.clear();
    const std::string prefix = "mcp__";
    if (qualified.rfind(prefix, 0) != 0) return false;
    const size_t sep = qualified.find("__", prefix.size());
    if (sep == std::string::npos) return false;
    const std::string srv = qualified.substr(prefix.size(), sep - prefix.size());
    const std::string t = qualified.substr(sep + 2);
    if (srv.empty() || t.empty()) return false;
    server = FromUtf8(srv);
    tool = t;
    return true;
}

// ── tools/call 结果解析 ───────────────────────────────────────────

std::wstring McpImageSpoolDir() {
    return AppDir() + L"\\agent_mcp_images";
}

namespace {

/// 递归删「太老」的文件（详见 agent_mcp.h 的说明）。
/// ⚠ 只按**最后写入时间**判，且未来时间戳一律不删（时钟不可信时宁可不删）。
int SweepMcpImagesRec(const std::wstring& dir, ULONGLONG ageLimit, ULONGLONG now,
                      std::vector<std::wstring>* removed) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int deleted = 0;
    do {
        if (fd.cFileName[0] == L'.'
            && (fd.cFileName[1] == 0 || (fd.cFileName[1] == L'.' && fd.cFileName[2] == 0))) {
            continue;
        }
        const std::wstring full = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            deleted += SweepMcpImagesRec(full, ageLimit, now, removed);
            RemoveDirectoryW(full.c_str());   // 空了才删得掉，失败无所谓
            continue;
        }
        ULARGE_INTEGER w{};
        w.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        w.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        // ⚠ `now > w.QuadPart` 这个前置条件不能省：时钟不可信 / 文件时间戳被拨到未来时，
        //   `now - w` 会按 ULONGLONG 回绕成天文数字 ⇒ 把「新图」当「老图」删掉。
        //   A/B 验证过：去掉它 ⇒ AgentMcpSelfTest 的 `sweep_stale_mcp_images` 转红
        //   （futureKept=0, n=2）。
        if (now > w.QuadPart && (now - w.QuadPart) > ageLimit) {
            if (DeleteFileW(full.c_str())) {
                ++deleted;
                if (removed) removed->push_back(full);
            }
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return deleted;
}

}  // namespace

int SweepStaleMcpImagesIn(const std::wstring& rootDir, int retentionDays,
    std::vector<std::wstring>* removed) {
    if (rootDir.empty()) return 0;
    if (retentionDays < 1) retentionDays = 1;
    FILETIME nowFt{};
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER now{};
    now.LowPart = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;
    // FILETIME 的单位是 100ns。
    const ULONGLONG ageLimit =
        static_cast<ULONGLONG>(retentionDays) * 24ULL * 3600ULL * 10000000ULL;
    return SweepMcpImagesRec(rootDir, ageLimit, now.QuadPart, removed);
}

int SweepStaleMcpImages(std::vector<std::wstring>* removed) {
    return SweepStaleMcpImagesIn(McpImageSpoolDir(), kMcpImageRetentionDays, removed);
}

namespace {

/// mime → 扩展名。**只认宿主 `AgentIsImagePath` 认得的那些**
/// （png/jpg/jpeg/gif/bmp/webp/tif/tiff，见 src/agent_attachment.cpp）——
/// 落盘成它不认的扩展名等于白存：宿主不会把那张图送进请求，
/// 我们却已经在文本里写了「附图」，模型会等一张永远不来的图。
/// 认不出就返回空，调用方改为留占位说明。
std::wstring ExtForImageMime(const std::string& mime) {
    std::string m;
    m.reserve(mime.size());
    for (char c : mime) m.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    const size_t semi = m.find(';');
    if (semi != std::string::npos) m.resize(semi);
    while (!m.empty() && (m.back() == ' ' || m.back() == '\t')) m.pop_back();
    if (m == "image/png") return L".png";
    if (m == "image/jpeg" || m == "image/jpg") return L".jpg";
    if (m == "image/gif") return L".gif";
    if (m == "image/bmp" || m == "image/x-ms-bmp") return L".bmp";
    if (m == "image/webp") return L".webp";
    if (m == "image/tiff") return L".tiff";
    return L"";
}

/// server 名进文件名前必须净化：它来自用户配置，可能含 `\` / `..`
/// ⇒ 直接拼进路径就是任意写。
std::wstring SanitizeForFileName(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if ((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z')
            || (c >= L'A' && c <= L'Z') || c == L'_' || c == L'-') {
            out.push_back(c);
        } else {
            out.push_back(L'_');
        }
        if (out.size() >= 40) break;
    }
    if (out.empty()) out = L"server";
    return out;
}

bool WriteBinaryFileAll(const std::wstring& path, const std::vector<uint8_t>& bytes) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    size_t off = 0;
    bool ok = true;
    while (off < bytes.size()) {
        const DWORD chunk = static_cast<DWORD>(
            (std::min)(bytes.size() - off, static_cast<size_t>(1u << 20)));
        DWORD written = 0;
        if (!WriteFile(h, bytes.data() + off, chunk, &written, nullptr)
            || written != chunk) {
            ok = false;
            break;
        }
        off += written;
    }
    CloseHandle(h);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

}  // namespace

bool McpExtractToolResult(const std::string& resultJson, const std::wstring& imageOutDir,
    McpToolContent& out, std::wstring& err) {
    out.text.clear();
    out.imageFiles.clear();
    err.clear();
    json j;
    try {
        j = json::parse(resultJson);
    } catch (const json::parse_error&) {
        err = L"tools/call 结果不是合法 JSON";
        return false;
    }
    if (!j.is_object()) {
        err = L"tools/call 结果不是对象";
        return false;
    }
    const bool isError = j.contains("isError") && j["isError"].is_boolean()
        && j["isError"].get<bool>();
    std::wstring text;
    size_t imageSeq = 0;
    if (j.contains("content") && j["content"].is_array()) {
        for (const auto& c : j["content"]) {
            if (!c.is_object()) continue;
            const std::string type = c.contains("type") && c["type"].is_string()
                ? c["type"].get<std::string>() : std::string();
            if (type == "text" && c.contains("text") && c["text"].is_string()) {
                if (!text.empty()) text += L"\n";
                text += FromUtf8(c["text"].get<std::string>());
                continue;
            }
            if (type.empty()) continue;

            // 图片：能落盘就落盘并打标记，让宿主把它送进下一轮请求。
            if (type == "image" && !imageOutDir.empty()
                && c.contains("data") && c["data"].is_string()) {
                const std::string mime = c.contains("mimeType") && c["mimeType"].is_string()
                    ? c["mimeType"].get<std::string>() : std::string();
                const std::wstring ext = ExtForImageMime(mime);
                if (!ext.empty()) {
                    const std::vector<uint8_t> bytes =
                        qst::base64::Decode(c["data"].get<std::string>());
                    if (!bytes.empty()) {
                        ++imageSeq;
                        const std::wstring path = imageOutDir + L"\\mcp_img_"
                            + std::to_wstring(GetTickCount64()) + L"_"
                            + std::to_wstring(imageSeq) + ext;
                        if (WriteBinaryFileAll(path, bytes)) {
                            if (!text.empty()) text += L"\n";
                            text += L"[[AGENT_IMG:" + path + L"]]";
                            out.imageFiles.push_back(path);
                            continue;
                        }
                    }
                    if (!text.empty()) text += L"\n";
                    text += L"[图片内容解码/落盘失败（" + FromUtf8(mime) + L"）]";
                    continue;
                }
                // 认不出的图片格式（svg/x-icon…）：**不许**存成 .png 假装能看图
                // （宿主会把它当位图解码失败），也**不许**悄悄丢 —— 说清楚是什么格式。
                if (!text.empty()) text += L"\n";
                text += L"[图片内容："
                    + FromUtf8(mime.empty() ? std::string("未知格式") : mime)
                    + L"，本工具不支持该格式（无法交给识图模型）；"
                      L"需要看图请让用户直接打开文件]";
                continue;
            }

            // 其余非文本内容**不能静默丢弃** —— 模型会以为「什么都没返回」，
            // 然后开始瞎猜。明确说一句「有内容但没转成文字」。
            if (!text.empty()) text += L"\n";
            text += L"[非文本内容：" + FromUtf8(type)
                + L"，本工具只回传文本，如需图片请让用户直接看文件]";
        }
    }
    if (text.empty()) text = isError ? L"（服务端报错但没有返回说明）" : L"（无返回内容）";
    out.text = text;
    if (isError) {
        err = L"MCP 工具执行失败";
        return false;
    }
    return true;
}

// ── 进程层 ───────────────────────────────────────────────────────

namespace {

/// 建一个「最后一个句柄关闭时杀掉所有成员」的 Job —— 孤儿防护，见 Start() 的注释。
/// 失败返回 nullptr，调用方按**降级**处理（功能照常，只是没有孤儿防护），
/// 绝不能因此让 Start 失败。
HANDLE CreateKillOnCloseJob() {
    HANDLE hJob = CreateJobObjectW(nullptr, nullptr);
    if (!hJob) return nullptr;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &info, sizeof(info))) {
        CloseHandle(hJob);
        return nullptr;
    }
    return hJob;
}

}  // namespace

struct McpStdioClient::Impl {
    HANDLE hProc = nullptr;
    HANDLE hJob = nullptr;      ///< 孤儿防护（KILL_ON_JOB_CLOSE），见 Start()
    HANDLE hStdinWrite = nullptr;
    HANDLE hStdoutRead = nullptr;
    std::thread reader;
    std::atomic_bool stop{false};
    /// ★ 单通道串行锁：MCP stdio 是「一问一答」协议，同一个 client 上**同时只能有一个
    ///   在途请求**。为什么必须有：`ReadResponse` 会把「别人的 id」的响应从队列里
    ///   **pop 掉并丢弃**（`Ignore → continue`）⇒ 两个并发调用会互相偷响应，
    ///   输的那一方一直等到**超时**（180s）才返回。
    ///   ⚠ 产品当前是**串行**调用工具的（`agent_core.cpp` 的 `tool_calls` 循环），
    ///   所以这个 bug 今天打不到；但**别把「调用方串行」当隐含前提** ——
    ///   一旦有人并行化工具调用，它就会变成「莫名其妙卡 3 分钟」。
    ///   `nextId++` 也靠它串行化（三个入口都持锁）。
    std::mutex callMu;
    std::mutex mu;
    std::condition_variable cv;
    std::deque<std::string> lines;
    std::wstring readErr;
    std::wstring jobDiag;       ///< 孤儿防护没生效时的诊断（别静默）
    int nextId = 1;
    std::wstring serverName;
    std::wstring imageDir;      ///< 附图落盘目录（懒建，每 client 一次）
    bool imageDirReady = false;
};

McpStdioClient::McpStdioClient() : impl_(new Impl()) {}

McpStdioClient::~McpStdioClient() { Stop(); }

int McpStdioClient::NextId() { return impl_->nextId++; }

bool McpStdioClient::OrphanGuardActive(std::wstring& diag) const {
    diag.clear();
    if (!impl_) {
        diag = L"client 未构造";
        return false;
    }
    if (impl_->hJob == nullptr) {
        diag = impl_->jobDiag.empty() ? L"未启动（Job 未创建）" : impl_->jobDiag;
        return false;
    }
    if (impl_->hProc == nullptr) {
        diag = L"子进程未启动";
        return false;
    }
    // ⚠ 必须**真去查子进程在不在 Job 里**，不能只看「hJob 非空 + jobDiag 为空」。
    //   第一版就是那么写的，结果 A/B（短路掉 AssignProcessToJobObject 的调用）
    //   **照样返回 true** —— 假绿。判据要盯**被保护的对象**，不是保护者的句柄。
    BOOL inJob = FALSE;
    if (!IsProcessInJob(impl_->hProc, impl_->hJob, &inJob)) {
        diag = L"IsProcessInJob 查询失败（错误码 " + std::to_wstring(GetLastError()) + L"）";
        return false;
    }
    if (!inJob) {
        diag = L"子进程不在 Job 里（孤儿防护未生效）";
        return false;
    }
    if (!impl_->jobDiag.empty()) {
        diag = impl_->jobDiag;
        return false;
    }
    return true;
}

HANDLE McpStdioClient::CreateKillOnCloseJobForTest() {
    return CreateKillOnCloseJob();
}

bool McpStdioClient::IsRunning() const {
    return impl_ && impl_->hProc != nullptr && !impl_->stop.load();
}

bool McpStdioClient::Start(const McpServerConfig& cfg, std::wstring& err) {
    err.clear();
    lastError_.clear();
    if (IsRunning()) return true;
    if (cfg.command.empty()) {
        err = L"server 配置缺少 command";
        return false;
    }

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE childStdinRead = nullptr, parentStdinWrite = nullptr;
    HANDLE parentStdoutRead = nullptr, childStdoutWrite = nullptr;
    if (!CreatePipe(&childStdinRead, &parentStdinWrite, &sa, 0)) {
        err = L"创建 stdin 管道失败";
        return false;
    }
    if (!CreatePipe(&parentStdoutRead, &childStdoutWrite, &sa, 0)) {
        CloseHandle(childStdinRead);
        CloseHandle(parentStdinWrite);
        err = L"创建 stdout 管道失败";
        return false;
    }
    // 父进程这一侧不继承，否则子进程句柄表里会多出自己管道的副本 ⇒ 读端永不 EOF
    SetHandleInformation(parentStdinWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(parentStdoutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = childStdinRead;
    si.hStdOutput = childStdoutWrite;
    // stderr 直接丢给 NUL：MCP 服务端的日志走 stderr，混进 stdout 会破坏分帧
    HANDLE hNul = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &sa, OPEN_EXISTING, 0, nullptr);
    si.hStdError = (hNul == INVALID_HANDLE_VALUE) ? childStdoutWrite : hNul;

    std::wstring cmdLine = BuildCommandLine(cfg);
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    PROCESS_INFORMATION pi{};
    // ★ 用 CREATE_SUSPENDED 起来：**先挂进 Job Object 再放行**。
    //   否则子进程可能在 Assign 之前就 spawn 出孙进程（Node 起渲染器 / 子 worker），
    //   那些孙进程不受 Job 管 ⇒ 宿主死后照样变孤儿。
    const BOOL ok = CreateProcessW(
        nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
        nullptr, cfg.cwd.empty() ? nullptr : cfg.cwd.c_str(),
        &si, &pi);

    CloseHandle(childStdinRead);
    CloseHandle(childStdoutWrite);
    if (hNul != INVALID_HANDLE_VALUE) CloseHandle(hNul);

    if (!ok) {
        CloseHandle(parentStdinWrite);
        CloseHandle(parentStdoutRead);
        err = L"启动 MCP server 失败（找不到程序？）：" + cfg.command;
        lastError_ = err;
        return false;
    }
    // ★★ 孤儿防护（`Stop()` 是**优雅**路径，管不了崩溃）：
    //   宿主被强杀 / 崩溃时 `Stop()` 根本没机会跑 ⇒ 子进程会一直挂着。
    //   实测形态：`genoffice mcp` 是个 Node 进程（几百 MB 常驻 + 持有管道），
    //   崩一次留一个，用户任务管理器里会攒出一串看不懂的 node.exe。
    //   挂到 `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 的 Job 上：只要本进程持有的
    //   **最后一个 Job 句柄**消失（正常退出 / 崩溃 / TerminateProcess 都一样，
    //   因为内核会回收句柄），Job 内所有进程连同后代一起被杀。
    impl_->hJob = CreateKillOnCloseJob();
    impl_->jobDiag.clear();
    if (impl_->hJob) {
        if (!AssignProcessToJobObject(impl_->hJob, pi.hProcess)) {
            // Windows 8+ 支持嵌套 Job，正常不该失败；失败只意味着**没有孤儿防护**，
            // 功能照常 ⇒ 不阻塞启动，但必须留痕，别让「防护没生效」变成静默。
            impl_->jobDiag = L"AssignProcessToJobObject 失败（孤儿防护未生效）";
        }
    } else {
        impl_->jobDiag = L"CreateJobObject/SetInformationJobObject 失败（孤儿防护未生效）";
    }
    ResumeThread(pi.hThread);   // CREATE_SUSPENDED 起来了，必须放行
    CloseHandle(pi.hThread);
    impl_->hProc = pi.hProcess;
    impl_->hStdinWrite = parentStdinWrite;
    impl_->hStdoutRead = parentStdoutRead;
    impl_->serverName = cfg.name;
    impl_->imageDir.clear();
    impl_->imageDirReady = false;
    impl_->stop.store(false);
    impl_->readErr.clear();
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        impl_->lines.clear();
    }

    // 读线程：ReadFile 是**阻塞**的。唤醒方式见 Stop() —— 那里有实测出来的坑。
    Impl* p = impl_;
    impl_->reader = std::thread([p]() {
        std::string buf;
        char chunk[4096];
        while (!p->stop.load()) {
            DWORD n = 0;
            const BOOL rok = ReadFile(p->hStdoutRead, chunk, sizeof(chunk), &n, nullptr);
            if (!rok || n == 0) {
                std::lock_guard<std::mutex> lock(p->mu);
                p->readErr = L"MCP server 输出流已关闭";
                p->cv.notify_all();
                return;
            }
            buf.append(chunk, n);
            size_t pos = 0;
            while ((pos = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, pos);
                buf.erase(0, pos + 1);
                std::lock_guard<std::mutex> lock(p->mu);
                p->lines.push_back(std::move(line));
                p->cv.notify_all();
            }
        }
    });
    return true;
}

void McpStdioClient::Stop() {
    if (!impl_) return;
    impl_->stop.store(true);
    // ★ 让「正在等响应」的调用**立刻**返回，而不是干等到它自己的超时（最长 180s）。
    //   `ReadResponse` 每轮先查 readErr 再查 deadline，所以置上它就够了。
    //   ⚠ **绝不能**在这里去拿 `callMu` —— 那正是要打断的那个调用持有的锁，会死锁。
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        if (impl_->readErr.empty()) impl_->readErr = L"MCP 连接已关闭";
    }
    impl_->cv.notify_all();
    // ⚠⚠ 顺序是**实测**出来的，别按直觉改回「先关句柄再 join」。
    //
    // 症状（2026-09-23，AgentMcpSelfTest）：用例打印完 PASS 就再也不动，
    // 既没有下一个用例的输出、也没有超时失败 —— 因为卡在**上一个用例的 Stop()** 里。
    //
    // 根因：读线程正阻塞在同步 ReadFile 上，而「另一个线程 CloseHandle 正在被阻塞
    // 使用的句柄」在 Windows 上是**未定义行为** —— 实测读线程**不会**被唤醒，
    // 于是 reader.join() 永远等不到（CancelIoEx 才是取消挂起 I/O 的正规手段）。
    //
    // 正确顺序 = 先让子进程死（它的 stdout 写端随之关闭 ⇒ 我们的读端必然收到
    // ERROR_BROKEN_PIPE），再 CancelIoEx 兜掉「赖着不退」的情况，最后才 join：
    //   ① 关子进程 stdin —— 所有 stdio 服务端的标准生命周期：读到 EOF 就自己退；
    //   ② CancelIoEx 取消此刻挂起的读；
    //   ③ 等子进程退出，1s 不退就强杀（内核会关掉它持有的所有句柄）；
    //   ④ 再 CancelIoEx 一次（②与③之间读线程可能又发起了一次读）；
    //   ⑤ 此刻 join 一定返回：子进程已死，任何新的 ReadFile 都立刻拿到 broken pipe；
    //   ⑥ 读线程已退出，这时关读句柄才是安全的。
    if (impl_->hStdinWrite) {
        CloseHandle(impl_->hStdinWrite);
        impl_->hStdinWrite = nullptr;
    }
    if (impl_->hStdoutRead) CancelIoEx(impl_->hStdoutRead, nullptr);
    if (impl_->hProc) {
        if (WaitForSingleObject(impl_->hProc, 1000) != WAIT_OBJECT_0)
            TerminateProcess(impl_->hProc, 0);
    }
    if (impl_->hStdoutRead) CancelIoEx(impl_->hStdoutRead, nullptr);
    if (impl_->reader.joinable()) impl_->reader.join();
    if (impl_->hStdoutRead) {
        CloseHandle(impl_->hStdoutRead);
        impl_->hStdoutRead = nullptr;
    }
    if (impl_->hProc) {
        CloseHandle(impl_->hProc);
        impl_->hProc = nullptr;
    }
    // 子进程已死再收 Job（KILL_ON_JOB_CLOSE 会顺带杀掉 Job 里还没退的残留，
    // 比如 Node 起的孙进程）。⚠ 这个句柄**必须**在 Impl 生命周期内一直持有 ——
    // 提前关掉就等于提前触发 KILL_ON_JOB_CLOSE（那正是我们靠它防孤儿的机制）。
    if (impl_->hJob) {
        CloseHandle(impl_->hJob);
        impl_->hJob = nullptr;
    }
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->lines.clear();
    impl_->cv.notify_all();
}

bool McpStdioClient::SendLine(const std::string& line, std::wstring& err) {
    err.clear();
    if (!impl_->hStdinWrite) {
        err = L"MCP server 未启动";
        return false;
    }
    const std::string payload = StripNewlines(line) + "\n";
    DWORD written = 0;
    if (!WriteFile(impl_->hStdinWrite, payload.data(),
            static_cast<DWORD>(payload.size()), &written, nullptr)
        || written != payload.size()) {
        err = L"写入 MCP server 失败（进程可能已退出）";
        lastError_ = err;
        return false;
    }
    FlushFileBuffers(impl_->hStdinWrite);
    return true;
}

McpLineKind McpStdioClient::ReadResponse(int id, std::string& resultJson,
    std::wstring& err, int timeoutMs) {
    resultJson.clear();
    err.clear();
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    for (;;) {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(impl_->mu);
            for (;;) {
                if (!impl_->lines.empty()) {
                    line = std::move(impl_->lines.front());
                    impl_->lines.pop_front();
                    break;
                }
                if (!impl_->readErr.empty()) {
                    err = impl_->readErr;
                    lastError_ = err;
                    return McpLineKind::Error;
                }
                const ULONGLONG now = GetTickCount64();
                if (now >= deadline) {
                    err = L"等待 MCP 响应超时";
                    lastError_ = err;
                    return McpLineKind::Error;
                }
                impl_->cv.wait_for(lock,
                    std::chrono::milliseconds(
                        static_cast<long long>((std::min)(deadline - now, 500ULL))));
            }
        }
        std::wstring lineErr;
        const McpLineKind kind = McpParseResponseLine(line, id, resultJson, lineErr);
        if (kind == McpLineKind::Ignore) continue;   // 别人的响应 / 日志噪声
        if (kind == McpLineKind::Error) {
            err = lineErr;
            lastError_ = err;
        }
        return kind;
    }
}

bool McpStdioClient::Initialize(std::wstring& err, int timeoutMs) {
    std::lock_guard<std::mutex> serial(impl_->callMu);   // 单通道串行（见 Impl::callMu）
    err.clear();
    json params;
    params["protocolVersion"] = kProtocolVersion;
    params["capabilities"] = json::object();
    json info;
    info["name"] = "quickscripttool-agent";
    info["version"] = "1.0";
    params["clientInfo"] = info;

    const int id = NextId();
    if (!SendLine(McpBuildRequestLine(id, "initialize", params.dump()), err)) return false;
    std::string result;
    if (ReadResponse(id, result, err, timeoutMs) != McpLineKind::Result) {
        if (err.empty()) err = L"initialize 未成功";
        return false;
    }
    // 通知：不需要响应，失败也不致命（老 server 可能不认这个通知）
    std::wstring nerr;
    SendLine(McpBuildNotificationLine("notifications/initialized", ""), nerr);
    return true;
}

bool McpStdioClient::ListTools(std::vector<McpRemoteTool>& out, std::wstring& err,
    int timeoutMs) {
    std::lock_guard<std::mutex> serial(impl_->callMu);   // 单通道串行（见 Impl::callMu）
    out.clear();
    err.clear();
    const int id = NextId();
    if (!SendLine(McpBuildRequestLine(id, "tools/list", ""), err)) return false;
    std::string result;
    if (ReadResponse(id, result, err, timeoutMs) != McpLineKind::Result) {
        if (err.empty()) err = L"tools/list 未成功";
        return false;
    }
    return McpParseToolsList(result, out, err);
}

bool McpStdioClient::CallTool(const std::string& toolName, const std::string& argsJson,
    std::wstring& outText, std::wstring& err, int timeoutMs) {
    // 单通道串行（理由见 Impl::callMu）—— 锁在**参数解析之前**拿，
    // 保证「进入 CallTool 的顺序」= 「请求真正发出的顺序」。
    // 单通道串行（理由见 Impl::callMu）—— 锁在**参数解析之前**拿，
    // 保证「进入 CallTool 的顺序」= 「请求真正发出的顺序」。
    // ⚠ A/B 验证过：注释掉这一行 ⇒ AgentMcpSelfTest 的
    //   `concurrent_call_tool_is_serialized` 转红（bDurMs=0，B 不再等 A）。
    std::lock_guard<std::mutex> serial(impl_->callMu);
    outText.clear();
    err.clear();
    json params;
    params["name"] = toolName;
    json args;
    try {
        args = argsJson.empty() ? json::object() : json::parse(argsJson);
    } catch (const json::parse_error&) {
        err = L"工具参数不是合法 JSON";
        return false;
    }
    if (!args.is_object()) args = json::object();
    params["arguments"] = args;

    const int id = NextId();
    if (!SendLine(McpBuildRequestLine(id, "tools/call", params.dump()), err)) return false;
    std::string result;
    // 工具调用可能很久（渲染文档、转 PDF），超时给得比握手宽
    if (ReadResponse(id, result, err, timeoutMs) != McpLineKind::Result) {
        if (err.empty()) err = L"tools/call 未成功";
        return false;
    }
    std::wstring textErr;
    // 附图落盘目录：按 server 分一层，出问题时一眼看出是哪家的图。
    // 懒建（每个 client 只 EnsureDirectoryTree 一次）。
    if (!impl_->imageDirReady) {
        impl_->imageDir = McpImageSpoolDir() + L"\\"
            + SanitizeForFileName(impl_->serverName);
        EnsureDirectoryTree(impl_->imageDir);
        impl_->imageDirReady = true;
    }
    McpToolContent content;
    const bool ok = McpExtractToolResult(result, impl_->imageDir, content, textErr);
    outText = content.text;
    if (!ok) err = textErr;
    return ok;
}

// ── 助手侧装配 ───────────────────────────────────────────────────

bool McpCommandIsRunnable(const std::wstring& path) {
    const size_t dot = path.find_last_of(L'.');
    const size_t slash = path.find_last_of(L"\\/");
    if (dot == std::wstring::npos) return false;
    if (slash != std::wstring::npos && dot < slash) return false;   // 点在目录名里
    const std::wstring ext = LowerCopy(path.substr(dot));
    // .com/.scr 也是可执行映像，但 GenOffice 不会用；只认这三种，宁可漏也不误判。
    return ext == L".exe" || ext == L".cmd" || ext == L".bat";
}

// ── 关于「自动接入第三方办公引擎」：**已删除**（2026-09-23）────────────────────
//
// 这里原来有 `DetectGenOfficeCli()` + `MakeGenOfficeMcpServerConfig()`：探测机器上有没有
// 装 GenOffice，装了就往 MCP server 列表里**自动补一条**。
//
// **删掉的理由（产品原则，别再加回来）**：
//   本产品**不依赖任何别人的软件运行**。自动探测 + 自动接入会让产品在用户不知情的情况下
//   把某项能力外包给第三方：对方没装就能力缺失、装了版本不对就行为不同、卸载了还留着一条
//   指向空气的配置。这不是「可选的扩展点」，这是**隐式依赖**。
//
// 正确做法（对 GenOffice 这类开源实现的正确借鉴方式）：
//   借鉴它的**做法与思路**（工具层不调模型 / 字节保留式编辑 / 产物可验收），
//   在**我们自己的引擎里实现** —— 见 `src/ooxml/`（自研 OOXML 容器与文档层）。
//
// 通用 **MCP 客户端保留**（`mcp_servers.json`）：那是**厂商中立、用户显式配置**的扩展点，
// 用户想接什么自己写一行配置，产品不替任何人做决定、也不主动探测任何东西。

namespace {

std::mutex g_mcpMu;
bool g_mcpLoaded = false;
std::vector<std::unique_ptr<McpStdioClient>> g_mcpClients;
std::vector<AgentTool> g_mcpTools;
std::wstring g_mcpDiag;

AgentTool MakeMcpAgentTool(const std::wstring& serverName, McpStdioClient* client,
    const McpRemoteTool& rt) {
    AgentTool tool;
    tool.name = FromUtf8(McpQualifiedToolName(serverName, rt.name));
    std::wstring desc = L"[外部 MCP 工具] 来自 server「" + serverName + L"」的 "
        + FromUtf8(rt.name) + L"。";
    if (!rt.description.empty()) desc += L"\n" + FromUtf8(rt.description);
    tool.description = desc;
    tool.parameters_json = rt.inputSchema.empty()
        ? LR"({"type":"object","properties":{}})"
        : FromUtf8(rt.inputSchema);
    const std::string remoteName = rt.name;
    tool.execute = [client, remoteName](const std::wstring& paramsJson) -> std::wstring {
        std::wstring text, err;
        if (!client->CallTool(remoteName, ToUtf8(paramsJson), text, err)) {
            return L"[错误] " + (err.empty() ? std::wstring(L"外部工具调用失败") : err);
        }
        return text;
    };
    return tool;
}

/// 懒加载：第一次调用时读配置、起进程、列工具，之后走缓存。
/// 为什么懒加载而不是启动时加载：绝大多数用户没有外部 server，
/// 没必要在启动路径上多做一次磁盘 IO + 进程探测。
void EnsureMcpLoadedLocked() {
    if (g_mcpLoaded) return;
    g_mcpLoaded = true;

    std::vector<McpServerConfig> servers;
    std::wstring err;
    if (!LoadMcpServers(servers, err)) {
        g_mcpDiag = L"mcp_servers.json 解析失败：" + err;
        return;
    }

    // ⚠ 这里原来有一段「自动探测 GenOffice 并补一条 server」——**已删除**。
    //   理由见本文件上方「关于自动接入第三方办公引擎」的注释：
    //   产品不主动依赖、也不主动探测任何第三方软件。用户要接什么，自己写进
    //   `mcp_servers.json`（厂商中立的显式扩展点）。

    // ★总预算：本函数在 UI 线程上跑，不能让一条坏配置把界面冻住。
    const ULONGLONG deadline = GetTickCount64() + kMcpEnumerateBudgetMs;
    for (const auto& cfg : servers) {
        if (!cfg.enabled) continue;
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            g_mcpDiag += L"[" + cfg.name + L"] 跳过：枚举预算已用尽；";
            continue;
        }
        // 单步超时 = 剩余预算，但至少给 500ms（否则会把「慢」误报成「坏」）
        const int stepMs = static_cast<int>(
            (std::max)(static_cast<ULONGLONG>(500), deadline - now));

        auto client = std::make_unique<McpStdioClient>();
        std::wstring e;
        if (!client->Start(cfg, e)) {
            g_mcpDiag += L"[" + cfg.name + L"] 启动失败：" + e + L"；";
            continue;
        }
        if (!client->Initialize(e, stepMs)) {
            g_mcpDiag += L"[" + cfg.name + L"] 握手失败：" + e + L"；";
            client->Stop();
            continue;
        }
        std::vector<McpRemoteTool> remote;
        if (!client->ListTools(remote, e, stepMs)) {
            g_mcpDiag += L"[" + cfg.name + L"] 列工具失败：" + e + L"；";
            client->Stop();
            continue;
        }
        McpStdioClient* raw = client.get();
        g_mcpClients.push_back(std::move(client));
        for (const auto& rt : remote) {
            g_mcpTools.push_back(MakeMcpAgentTool(cfg.name, raw, rt));
        }
        g_mcpDiag += L"[" + cfg.name + L"] 已接入 " + std::to_wstring(remote.size())
            + L" 个工具；";
    }
}

}  // namespace

std::vector<AgentTool> CollectMcpAgentTools() {
    std::lock_guard<std::mutex> lock(g_mcpMu);
    EnsureMcpLoadedLocked();
    return g_mcpTools;
}

}  // namespace agent
}  // namespace qst
