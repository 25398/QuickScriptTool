// ──────────────────────────────────────────────────────────────────
// ocr_winrt.cpp — 系统自带（WinRT）OCR 后端实现
// 见 ocr_backend.h 的说明。
//
// 实测（2026-09-19，中文 Windows 11）：
//   OcrEngine::AvailableRecognizerLanguages() → zh-Hans-CN
//   OcrEngine::TryCreateFromUserProfileLanguages() → 非空
// 也就是说**零安装**就能做中文 OCR —— 这是导出 EXE 能把 OCR 做成
// 「不依赖 Python」的关键。
// ──────────────────────────────────────────────────────────────────

#include "ocr_backend.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <mutex>
#include <vector>

#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

namespace {

/// 步骤日志：OCR 走 WinRT 时若进程被硬杀（SEH / 非 C++ 异常），
/// 这个文件就是唯一的现场证据 —— 能看到死在"哪一步"。
/// 默认**不写**（每次 OCR 都落盘太吵）。排查时设环境变量 `QST_OCR_DEBUG=1`
/// 再运行，日志落在 exe 旁的 `ocr_winrt_debug.log` —— 进程若被 SEH 硬杀，
/// 这个文件是唯一能看出"死在哪一步"的现场证据。
bool StepLogEnabled() {
    static const bool on = [] {
        wchar_t buf[8]{};
        return GetEnvironmentVariableW(L"QST_OCR_DEBUG", buf, 8) > 0;
    }();
    return on;
}

void StepLog(const char* step) {
    if (!StepLogEnabled()) return;
    static std::mutex mu;
    std::lock_guard<std::mutex> lock(mu);
    wchar_t buf[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, buf, MAX_PATH) == 0) return;
    std::wstring dir(buf);
    const auto slash = dir.find_last_of(L"\\");
    if (slash == std::wstring::npos) return;
    dir.resize(slash);
    HANDLE h = CreateFileW((dir + L"\\ocr_winrt_debug.log").c_str(),
        FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[256]{};
    const int n = sprintf_s(line, "[%02d:%02d:%02d] %s\r\n",
        st.wHour, st.wMinute, st.wSecond, step);
    SetFilePointer(h, 0, nullptr, FILE_END);
    DWORD put = 0;
    if (n > 0) WriteFile(h, line, static_cast<DWORD>(n), &put, nullptr);
    CloseHandle(h);
}

std::atomic<int> g_backend{ static_cast<int>(OcrBackend::Python) };

/// WinRT 要求调用线程已初始化 apartment。OCR 可能跑在引擎的工作线程上，
/// 所以用 thread_local 而不是 call_once —— 每个线程都要自己初始化一次。
thread_local bool t_apartmentReady = false;

void EnsureApartment() {
    if (t_apartmentReady) return;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
        // RPC_E_CHANGED_MODE：本线程已是 STA（引擎/壳的主线程常见）。
        // 对只做「创建识别器 + 同步等待一次识别」的用法来说，沿用现有 apartment 也能跑。
    }
    t_apartmentReady = true;
}

using winrt::Windows::Media::Ocr::OcrEngine;
using winrt::Windows::Graphics::Imaging::BitmapAlphaMode;
using winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
using winrt::Windows::Graphics::Imaging::SoftwareBitmap;

/// 建识别器：先按用户语言，再退到任意可用语言。
/// 为什么要有回退：用户语言是英文的系统上，中文识别器仍然可用（装了语言包的话），

/// 直接 `TryCreateFromUserProfileLanguages` 失败就放弃会误判成"不可用"。
OcrEngine MakeEngine(std::wstring* reason) {
    try {
        if (auto e = OcrEngine::TryCreateFromUserProfileLanguages()) return e;
        for (auto const& lang : OcrEngine::AvailableRecognizerLanguages()) {
            if (auto e = OcrEngine::TryCreateFromLanguage(lang)) return e;
        }
        if (reason) {
            *reason = L"系统没有安装任何 OCR 语言包。\n"
                      L"可在「设置 → 时间和语言 → 语言和区域」里为中文添加「光学字符识别」。";
        }
    } catch (winrt::hresult_error const& e) {
        if (reason) *reason = std::wstring(L"系统 OCR 初始化失败：") + e.message().c_str();
    } catch (...) {
        if (reason) *reason = L"系统 OCR 初始化失败（未知错误）";
    }
    return nullptr;
}

/// 只保留数字与常见分隔符（对应产品的「纯数字」模式）。
std::wstring DigitsOnly(const std::wstring& in) {
    std::wstring out;
    out.reserve(in.size());
    for (wchar_t ch : in) {
        if ((ch >= L'0' && ch <= L'9') || ch == L'.' || ch == L',' || ch == L':'
            || ch == L'-' || ch == L'+' || ch == L'%' || ch == L'/') {
            out.push_back(ch);
        }
    }
    return out;
}

}  // namespace

/// 是否 CJK 字符（汉字 / 假名 / 全角标点与字母数字）。
bool IsCjk(wchar_t ch) {
    const unsigned c = static_cast<unsigned>(ch);
    return (c >= 0x3000 && c <= 0x303F)      // CJK 标点
        || (c >= 0x3040 && c <= 0x30FF)      // 平假名 / 片假名
        || (c >= 0x3400 && c <= 0x4DBF)      // 扩展 A
        || (c >= 0x4E00 && c <= 0x9FFF)      // 基本区
        || (c >= 0xF900 && c <= 0xFAFF)      // 兼容表意
        || (c >= 0xFF00 && c <= 0xFFEF);     // 全角形式
}

/// 见 ocr_backend.h 的说明（CJK 之间不加空格）。
std::wstring JoinOcrWords(const std::vector<std::wstring>& words) {
    std::wstring out;
    for (const auto& w : words) {
        if (w.empty()) continue;
        if (!out.empty() && !IsCjk(out.back()) && !IsCjk(w.front())) out += L' ';
        out += w;
    }
    return out;
}


namespace {
std::atomic<void (*)(const std::wstring&)> g_ocrDiagSink{ nullptr };
}

void SetOcrDiagnosticSink(void (*sink)(const std::wstring&)) {
    g_ocrDiagSink.store(sink, std::memory_order_release);
}

/// 供 ocr_engine.cpp 调用：把一行摘要送给已安装的 sink（没装就什么都不做）。
void EmitOcrDiagnostic(const std::wstring& line) {
    auto fn = g_ocrDiagSink.load(std::memory_order_acquire);
    if (!fn) return;
    // sink 自身出问题绝不能影响识别结果
    try {
        fn(line);
    } catch (...) {
    }
}

std::vector<OcrBackend> OcrBackendAttemptOrder(OcrBackend pref) {
    switch (pref) {
    case OcrBackend::WinRt: return { OcrBackend::WinRt };
    case OcrBackend::Auto:  return { OcrBackend::Python, OcrBackend::WinRt };
    case OcrBackend::Python:
    default:                return { OcrBackend::Python };
    }
}

void SetOcrBackendPreference(OcrBackend backend) {
    g_backend.store(static_cast<int>(backend), std::memory_order_release);
}

OcrBackend OcrBackendPreference() {
    const int v = g_backend.load(std::memory_order_acquire);
    if (v < 0 || v > 2) return OcrBackend::Python;
    return static_cast<OcrBackend>(v);
}

bool WinRtOcrAvailable(std::wstring* reason) {
    EnsureApartment();
    std::wstring why;
    if (MakeEngine(&why)) {
        if (reason) reason->clear();
        return true;
    }
    if (reason) *reason = why.empty() ? L"系统 OCR 不可用" : why;
    return false;
}

OcrEngineOutput RunWinRtOcr(HBITMAP bitmap, bool digitsOnly) {
    StepLog("enter RunWinRtOcr");
    OcrEngineOutput output;
    if (!bitmap) {
        output.error = L"无法截取识别区域";
        return output;
    }
    StepLog("before EnsureApartment");
    EnsureApartment();
    StepLog("after EnsureApartment");

    std::wstring why;
    OcrEngine engine = MakeEngine(&why);
    if (!engine) {
        output.error = why.empty() ? L"系统 OCR 不可用" : why;
        return output;
    }

    // HBITMAP → 32 位 BGRA（自顶向下）
    BITMAP bm{};
    if (!GetObjectW(bitmap, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        output.error = L"无法读取截图尺寸";
        return output;
    }
    const int w = bm.bmWidth;
    const int h = bm.bmHeight;
    {
        char b[64]{};
        sprintf_s(b, "bitmap %dx%d", w, h);
        StepLog(b);
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // 负数 = 自顶向下，与 SoftwareBitmap 一致
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC dc = GetDC(nullptr);
    if (!dc) {
        output.error = L"无法获取屏幕 DC";
        return output;
    }
    const int lines = GetDIBits(dc, bitmap, 0, h, pixels.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    if (lines <= 0) {
        output.error = L"无法读取截图像素";
        return output;
    }
    // WinRT 的 Premultiplied 模式要求 alpha 参与运算；截图 alpha 常为 0，
    // 直接传会得到全透明位图 → 识别不出任何东西。统一补成不透明。
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 0xFF;
    StepLog("GetDIBits ok, alpha forced opaque");

    try {
        // CreateCopyFromBuffer 要的是 IBuffer，不是裸指针 —— 用 DataWriter 包一层
        winrt::Windows::Storage::Streams::DataWriter writer;
        writer.WriteBytes(winrt::array_view<uint8_t const>(
            pixels.data(), static_cast<uint32_t>(pixels.size())));
        SoftwareBitmap sb = SoftwareBitmap::CreateCopyFromBuffer(
            writer.DetachBuffer(), BitmapPixelFormat::Bgra8, w, h,
            BitmapAlphaMode::Premultiplied);
        StepLog("before RecognizeAsync");
        auto result = engine.RecognizeAsync(sb).get();
        StepLog("after RecognizeAsync");

        for (auto const& line : result.Lines()) {
            // 逐词收集：文本自己拼（见 JoinOcrWords），行框取词框并集
            std::vector<std::wstring> words;
            int x1 = INT32_MAX, y1 = INT32_MAX, x2 = 0, y2 = 0;
            for (auto const& word : line.Words()) {
                words.push_back(word.Text().c_str());
                const auto r = word.BoundingRect();
                x1 = std::min(x1, static_cast<int>(r.X));
                y1 = std::min(y1, static_cast<int>(r.Y));
                x2 = std::max(x2, static_cast<int>(r.X + r.Width));
                y2 = std::max(y2, static_cast<int>(r.Y + r.Height));
            }
            std::wstring text = JoinOcrWords(words);
            if (text.empty()) text = line.Text().c_str();   // 拿不到词时退回整行
            if (text.empty()) continue;
            if (x2 <= x1 || y2 <= y1) {
                x1 = 0; y1 = 0; x2 = w; y2 = h;  // 拿不到词框时退化成整幅，避免丢行
            }

            if (digitsOnly) {
                text = DigitsOnly(text);
                if (text.empty()) continue;
            }

            OcrTextLine out;
            out.text = text;
            out.x1 = x1;
            out.y1 = y1;
            out.x2 = x2;
            out.y2 = y2;
            // WinRT 不暴露置信度；给 1.0（"确定"）以免被下游的阈值逻辑误过滤。
            out.confidence = 1.0;
            output.lines.push_back(std::move(out));
        }
        output.success = true;
        StepLog("lines collected");
        if (output.lines.empty()) {
            // 识别成功但没文字：不是错误，调用方按"没找到"处理
            output.error.clear();
        }
    } catch (winrt::hresult_error const& e) {
        StepLog("hresult_error caught");
        output.error = std::wstring(L"系统 OCR 识别失败：") + e.message().c_str();
    } catch (...) {
        StepLog("unknown exception caught");
        output.error = L"系统 OCR 识别失败（未知错误）";
    }
    StepLog("leave RunWinRtOcr");
    return output;
}
