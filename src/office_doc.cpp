#include "office_doc.h"

#include "utils.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

namespace {

using json = nlohmann::json;

/// 运行 PowerShell 抓 stdout（带超时与大小上限）。
/// 注意：ocr_engine.cpp 里那份是无上限、无限等待的（OCR 进程自己管超时），
/// 读文档可能遇到坏文件/大文件，必须能掐断，否则会卡住整个 AI 动作。
bool RunCaptureWithTimeout(const std::wstring& commandLine, std::string& out, DWORD timeoutMs,
    size_t maxBytes) {
    out.clear();
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return false;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmd(commandLine.begin(), commandLine.end());
    cmd.push_back(L'\0');
    const BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!started) {
        CloseHandle(readPipe);
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeoutMs);
    std::thread reader([&]() {
        char buf[4096];
        DWORD n = 0;
        for (;;) {
            const BOOL ok = ReadFile(readPipe, buf, sizeof(buf), &n, nullptr);
            if (!ok || n == 0) break;
            if (out.size() < maxBytes) {
                const size_t room = maxBytes - out.size();
                out.append(buf, buf + (n < room ? n : room));
            }
            // 超量后继续读干管道，避免子进程写阻塞
        }
    });

    bool timedOut = false;
    for (;;) {
        const DWORD wait = WaitForSingleObject(pi.hProcess, 50);
        if (wait == WAIT_OBJECT_0) break;
        if (std::chrono::steady_clock::now() >= deadline) {
            timedOut = true;
            TerminateProcess(pi.hProcess, 1);
            break;
        }
    }
    reader.join();
    CloseHandle(readPipe);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (timedOut) return false;
    return true;
}

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L"\\" + name;
}

std::wstring ParentDir(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool FileExistsPath(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

/// 路径去引号并转义成可安全放进命令行的形式。
/// ★必须用 Windows 标准的**双引号**：`powershell -File` 不认单引号参数
///（实测写成 -File 'C:\...' 会报 "The given path's format is not supported"，
///  然后在 stdout 打出 PowerShell 横幅 → 宿主 JSON 解析失败，极难排查）。
std::wstring QuoteWinArg(const std::wstring& s) {
    std::wstring t = s;
    // 末尾反斜杠会「吃掉」闭合引号，先剥掉（我们只传文件路径，不会是目录）
    while (!t.empty() && t.back() == L'\\') t.pop_back();
    std::wstring out = L"\"";
    for (const wchar_t c : t) {
        if (c == L'"') out += L'\\';
        out += c;
    }
    out += L"\"";
    return out;
}

}  // namespace

std::wstring OfficeDocScriptPath() {
    const std::wstring exeDir = AppDir();
    // ① 随包分发（首选）
    const std::wstring deployed = JoinPath(JoinPath(exeDir, L"office"), L"read_doc.ps1");
    if (FileExistsPath(deployed)) return deployed;
    // ② 便携包/绿色版可能把资源放在 AppDir 上一级
    const std::wstring up = JoinPath(JoinPath(ParentDir(exeDir), L"office"), L"read_doc.ps1");
    if (FileExistsPath(up)) return up;
    // ③ 开发机：build\Release → 仓库根 tools\office\read_doc.ps1
    const std::wstring repo = JoinPath(JoinPath(JoinPath(ParentDir(exeDir), L".."), L"tools\\office"),
        L"read_doc.ps1");
    if (FileExistsPath(repo)) return repo;
    return std::wstring();
}

bool IsSupportedOfficeDocument(const std::wstring& path, std::wstring* formatOut) {
    std::wstring ext;
    const size_t dot = path.find_last_of(L'.');
    if (dot != std::wstring::npos) {
        ext = path.substr(dot + 1);
        for (auto& c : ext)
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    static const wchar_t* kSupported[] = {
        L"xlsx", L"xlsm", L"xls", L"xlsb", L"csv", L"tsv", L"docx", L"docm", L"doc",
        L"pptx", L"ppt", L"pdf", L"txt", L"md", L"json", L"log", L"xml", L"ini", L"sql",
    };
    for (auto* k : kSupported) {
        if (ext == k) {
            if (formatOut) *formatOut = ext;
            return true;
        }
    }
    if (formatOut) formatOut->clear();
    return false;
}

std::wstring OfficeDocFormatLabel(const std::wstring& format) {
    if (format == L"xlsx" || format == L"xlsm" || format == L"xlsb") return L"Excel 工作簿";
    if (format == L"xls") return L"Excel 97-2003 工作簿";
    if (format == L"csv" || format == L"tsv") return L"表格文本";
    if (format == L"docx" || format == L"docm") return L"Word 文档";
    if (format == L"doc") return L"Word 97-2003 文档";
    if (format == L"pptx") return L"PowerPoint 演示文稿";
    if (format == L"ppt") return L"PowerPoint 97-2003 演示文稿";
    if (format == L"pdf") return L"PDF";
    return L"文本文件";
}

OfficeDocResult ReadOfficeDocument(const std::wstring& path, int maxChars, int pageLimit,
    const std::wstring& imageDir) {
    OfficeDocResult out;
    const std::wstring trimmed = Trim(path);
    if (trimmed.empty()) {
        out.error = L"路径为空。";
        return out;
    }
    std::wstring format;
    if (!IsSupportedOfficeDocument(trimmed, &format)) {
        out.error = L"暂不支持这种文件类型（支持 xlsx/xlsm/xls/csv/docx/doc/pptx/ppt/pdf/txt 等）。"
                    L"若是其它格式，先用 runCommand 转换（见 lookupMacroAction(section=office)）。";
        return out;
    }
    out.format = format;
    if (!FileExistsPath(trimmed)) {
        out.error = L"文件不存在：" + trimmed
            + L"。桌面路径请先用 resolveSystemPath(folder=desktop) 取真实路径。";
        return out;
    }
    const std::wstring script = OfficeDocScriptPath();
    if (script.empty()) {
        out.error = L"找不到文档提取脚本 office\\read_doc.ps1（安装包应随附）。"
                    L"可用 runCommand 自行读取（见 lookupMacroAction(section=office)）。";
        return out;
    }
    if (maxChars < 200) maxChars = 200;
    if (maxChars > 60000) maxChars = 60000;
    if (pageLimit < 1) pageLimit = 1;
    if (pageLimit > 20) pageLimit = 20;

    std::wstring cmd = L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File ";
    cmd += QuoteWinArg(script);
    cmd += L" -Path " + QuoteWinArg(trimmed);
    cmd += L" -MaxChars " + std::to_wstring(maxChars);
    cmd += L" -PageLimit " + std::to_wstring(pageLimit);
    if (!imageDir.empty()) cmd += L" -ImageDir " + QuoteWinArg(imageDir);

    std::string stdoutText;
    const bool ran = RunCaptureWithTimeout(cmd, stdoutText, 60000, 2u * 1024u * 1024u);
    if (!ran || stdoutText.empty()) {
        out.error = L"读取超时或进程启动失败（文件可能损坏、被占用，或过大）。"
                    L"可先用 runCommand+Office COM 另存为副本再读，或直接 openFile 打开后用视觉读取。";
        return out;
    }
    // 脚本可能被 PowerShell 的横幅/告警污染：从后往前找第一行「{…}」整行 JSON
    std::string candidate = stdoutText;
    for (size_t end = stdoutText.find_last_of("\r\n");; ) {
        const size_t lineStart = (end == std::string::npos)
            ? 0 : stdoutText.find_last_of("\r\n", end - 1) + 1;
        const size_t lineEnd = (end == std::string::npos) ? stdoutText.size() : end;
        if (lineEnd > lineStart && stdoutText[lineStart] == '{') {
            candidate = stdoutText.substr(lineStart, lineEnd - lineStart);
            break;
        }
        if (lineStart == 0) break;
        end = (lineStart >= 1) ? lineStart - 1 : std::string::npos;
        if (end == std::string::npos) break;
    }
    json j;
    try {
        j = json::parse(candidate);
    } catch (...) {
        try {
            j = json::parse(stdoutText);
        } catch (...) {
            // 兜底：脚本若因环境原因按系统 ANSI(GBK) 输出，UTF-8 解析会失败 ——
            // 按 ANSI 解成宽字符再转回 UTF-8 重试一次（中文路径/文本很常见）。
            std::wstring wide;
            const int need = MultiByteToWideChar(CP_ACP, 0, stdoutText.c_str(),
                static_cast<int>(stdoutText.size()), nullptr, 0);
            if (need > 0) {
                wide.resize(static_cast<size_t>(need));
                MultiByteToWideChar(CP_ACP, 0, stdoutText.c_str(),
                    static_cast<int>(stdoutText.size()), wide.data(), need);
                try {
                    j = json::parse(ToUtf8(wide));
                } catch (const std::exception& e) {
                    out.error = L"提取脚本返回无法解析的结果：" + FromUtf8(std::string(e.what()))
                        + L"（原文：" + FromUtf8(stdoutText.substr(0, 200)) + L"）";
                    return out;
                }
            } else {
                out.error = L"提取脚本返回无法解析的结果（原文："
                    + FromUtf8(stdoutText.substr(0, 200)) + L"）";
                return out;
            }
        }
    }
    if (!j.is_object()) {
        out.error = L"提取脚本返回格式异常。";
        return out;
    }
    if (j.contains("ok") && j["ok"].is_boolean() && !j["ok"].get<bool>()) {
        out.error = j.contains("error") && j["error"].is_string()
            ? FromUtf8(j["error"].get<std::string>()) : std::wstring(L"读取失败。");
        return out;
    }
    out.ok = true;
    auto takeStr = [&](const char* key) -> std::wstring {
        if (j.contains(key) && j[key].is_string())
            return FromUtf8(j[key].get<std::string>());
        return std::wstring();
    };
    out.engine = takeStr("engine");
    out.text = takeStr("text");
    out.note = takeStr("note");
    if (j.contains("truncated") && j["truncated"].is_boolean())
        out.truncated = j["truncated"].get<bool>();
    if (j.contains("images") && j["images"].is_array()) {
        for (const auto& img : j["images"]) {
            if (img.is_string()) out.images.push_back(FromUtf8(img.get<std::string>()));
        }
    }
    return out;
}
