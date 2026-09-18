#include "opencv_runtime.h"

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>

namespace {

constexpr wchar_t kOpenCvDll[] = L"opencv_world4100.dll";

enum class State : int { Unknown = 0, Ready = 1, Missing = 2 };

std::atomic<int> g_state{static_cast<int>(State::Unknown)};
std::mutex g_reasonMu;
std::wstring g_reason = L"尚未探测 OpenCV";

void SetState(State s, const wchar_t* reason) {
    g_state.store(static_cast<int>(s), std::memory_order_release);
    std::lock_guard<std::mutex> lock(g_reasonMu);
    g_reason = reason ? reason : L"";
}

bool MediaFoundationPresent() {
    static const wchar_t* kMf[] = { L"MFPlat.DLL", L"MF.dll", L"MFReadWrite.dll" };
    for (const wchar_t* name : kMf) {
        HMODULE m = GetModuleHandleW(name);
        if (!m) m = LoadLibraryW(name);
        if (!m) return false;
    }
    return true;
}

std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full(path);
    const auto slash = full.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return {};
    return full.substr(0, slash);
}

}  // namespace

void MarkOpenCvUnavailable(const wchar_t* reason) {
    SetState(State::Missing, reason ? reason : L"opencv_world4100.dll 不可用");
}

bool TryInitOpenCv(std::wstring* reason) {
    const int cur = g_state.load(std::memory_order_acquire);
    if (cur == static_cast<int>(State::Ready)) {
        if (reason) reason->clear();
        return true;
    }
    if (cur == static_cast<int>(State::Missing)) {
        if (reason) {
            std::lock_guard<std::mutex> lock(g_reasonMu);
            *reason = g_reason;
        }
        return false;
    }

    if (!MediaFoundationPresent()) {
        SetState(State::Missing, L"系统缺少 Media Foundation（Win N/KN 需安装媒体功能包）");
        if (reason) {
            std::lock_guard<std::mutex> lock(g_reasonMu);
            *reason = g_reason;
        }
        return false;
    }

    HMODULE already = GetModuleHandleW(kOpenCvDll);
    if (already) {
        SetState(State::Ready, L"");
        if (reason) reason->clear();
        return true;
    }

    const std::wstring dir = ExeDir();
    const std::wstring beside = dir.empty() ? std::wstring(kOpenCvDll) : (dir + L"\\" + kOpenCvDll);
    if (GetFileAttributesW(beside.c_str()) == INVALID_FILE_ATTRIBUTES) {
        SetState(State::Missing, L"软件目录缺少 opencv_world4100.dll（可能被杀软隔离）");
        if (reason) {
            std::lock_guard<std::mutex> lock(g_reasonMu);
            *reason = g_reason;
        }
        return false;
    }

    HMODULE cv = LoadLibraryW(beside.c_str());
    if (!cv) {
        SetState(State::Missing, L"无法加载 opencv_world4100.dll（杀软隔离或依赖缺失）");
        if (reason) {
            std::lock_guard<std::mutex> lock(g_reasonMu);
            *reason = g_reason;
        }
        return false;
    }
    SetState(State::Ready, L"");
    if (reason) reason->clear();
    return true;
}

bool OpenCvAvailable() {
    const int cur = g_state.load(std::memory_order_acquire);
    if (cur == static_cast<int>(State::Ready)) return true;
    if (cur == static_cast<int>(State::Missing)) return false;
    return TryInitOpenCv(nullptr);
}

const wchar_t* OpenCvUnavailableMessage() {
    static thread_local std::wstring tls;
    std::lock_guard<std::mutex> lock(g_reasonMu);
    tls = g_reason.empty()
        ? L"找图引擎不可用（缺少 OpenCV）"
        : (L"找图引擎不可用：" + g_reason);
    return tls.c_str();
}
