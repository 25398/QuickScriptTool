// image_var_util.cpp
#include "image_var_util.h"

#include "image_match.h"
#include "utils.h"

#include <cwctype>

#include <objbase.h>
#include <shlobj.h>
#include <wincodec.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

namespace {

std::wstring ToLowerExt(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    const auto dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
        return L"";
    }
    std::wstring ext = path.substr(dot);
    for (wchar_t& ch : ext) ch = static_cast<wchar_t>(towlower(ch));
    return ext;
}

bool SaveBitmapViaWic(HBITMAP bitmap, const std::wstring& path, const GUID& containerFormat) {
    if (!bitmap || path.empty()) return false;

    BITMAP bm{};
    if (GetObjectW(bitmap, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0) {
        return false;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool needUninit = (hr == S_OK);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE) {
        return false;
    }

    bool ok = false;
    IWICImagingFactory* factory = nullptr;
    IWICBitmap* wicBmp = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;

    do {
        hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
        if (FAILED(hr) || !factory) break;

        hr = factory->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &wicBmp);
        if (FAILED(hr) || !wicBmp) break;

        hr = factory->CreateStream(&stream);
        if (FAILED(hr) || !stream) break;
        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (FAILED(hr)) break;

        hr = factory->CreateEncoder(containerFormat, nullptr, &encoder);
        if (FAILED(hr) || !encoder) break;
        hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
        if (FAILED(hr)) break;

        hr = encoder->CreateNewFrame(&frame, &props);
        if (FAILED(hr) || !frame) break;
        hr = frame->Initialize(props);
        if (FAILED(hr)) break;

        UINT w = 0, h = 0;
        hr = wicBmp->GetSize(&w, &h);
        if (FAILED(hr)) break;
        hr = frame->SetSize(w, h);
        if (FAILED(hr)) break;

        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&format);
        if (FAILED(hr)) break;

        hr = frame->WriteSource(wicBmp, nullptr);
        if (FAILED(hr)) break;
        hr = frame->Commit();
        if (FAILED(hr)) break;
        hr = encoder->Commit();
        if (FAILED(hr)) break;
        ok = true;
    } while (false);

    if (props) props->Release();
    if (frame) frame->Release();
    if (encoder) encoder->Release();
    if (stream) stream->Release();
    if (wicBmp) wicBmp->Release();
    if (factory) factory->Release();
    if (needUninit) CoUninitialize();
    return ok;
}

}  // namespace

bool LooksLikeFilePath(const std::wstring& s) {
    if (s.empty()) return false;
    if (s.find(L'\\') != std::wstring::npos || s.find(L'/') != std::wstring::npos) return true;
    if (s.size() >= 2 && ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z'))
        && s[1] == L':') {
        return true;
    }
    return false;
}

bool EnsureParentDir(const std::wstring& path) {
    if (path.empty()) return false;
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash == 0) return true;
    std::wstring dir = path.substr(0, slash);
    if (dir.empty()) return true;
    if (GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    const int rc = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS;
}

bool SaveHBitmapByExtension(HBITMAP bitmap, const std::wstring& path) {
    if (!bitmap || path.empty()) return false;
    if (!EnsureParentDir(path)) return false;

    const std::wstring ext = ToLowerExt(path);
    if (ext.empty() || ext == L".bmp") {
        return SaveBitmapToFile(bitmap, path);
    }
    if (ext == L".png") {
        return SaveBitmapViaWic(bitmap, path, GUID_ContainerFormatPng);
    }
    if (ext == L".jpg" || ext == L".jpeg") {
        return SaveBitmapViaWic(bitmap, path, GUID_ContainerFormatJpeg);
    }
    // Unknown extension → BMP bytes still (caller asked for that name).
    return SaveBitmapToFile(bitmap, path);
}

std::wstring ImageVarRuntimeDir() {
    return FindImagesDir() + L"\\_runtime";
}

std::wstring MakeImageVarTempPath(unsigned long long runId, const std::wstring& varName) {
    std::wstring safe = varName.empty() ? L"image" : varName;
    for (wchar_t& ch : safe) {
        if (!((ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z')
            || (ch >= L'0' && ch <= L'9') || ch == L'_')) {
            ch = L'_';
        }
    }
    return ImageVarRuntimeDir() + L"\\" + std::to_wstring(runId) + L"_" + safe + L".bmp";
}

std::wstring ResolveRuntimeImagePath(
    const std::wstring& token,
    const std::unordered_map<std::wstring, std::wstring>* imageVars) {
    std::wstring t = token;
    // trim
    while (!t.empty() && (t.front() == L' ' || t.front() == L'\t')) t.erase(t.begin());
    while (!t.empty() && (t.back() == L' ' || t.back() == L'\t')) t.pop_back();
    if (t.empty()) return L"";

    // Strip {var} braces if present.
    if (t.size() >= 2 && t.front() == L'{' && t.back() == L'}') {
        t = t.substr(1, t.size() - 2);
        while (!t.empty() && (t.front() == L' ' || t.front() == L'\t')) t.erase(t.begin());
        while (!t.empty() && (t.back() == L' ' || t.back() == L'\t')) t.pop_back();
    }

    if (LooksLikeFilePath(t)) {
        if (t.size() >= 2 && t[1] == L':') return t;
        if (t.size() >= 2 && t[0] == L'\\' && t[1] == L'\\') return t;
        return ResolveImagePath(t);
    }

    if (imageVars) {
        const auto it = imageVars->find(t);
        if (it != imageVars->end()) return it->second;
    }
    return L"";
}

void ClearImageVars(std::unordered_map<std::wstring, std::wstring>& imageVars) {
    const std::wstring runtime = ImageVarRuntimeDir();
    for (const auto& kv : imageVars) {
        if (kv.second.empty()) continue;
        if (_wcsnicmp(kv.second.c_str(), runtime.c_str(), runtime.size()) == 0) {
            DeleteFileW(kv.second.c_str());
        }
    }
    imageVars.clear();
}

bool GetImageFileSize(const std::wstring& path, int& outW, int& outH) {
    outW = outH = 0;
    if (path.empty()) return false;
    HBITMAP bmp = LoadBitmapFromFile(path);
    if (!bmp) return false;
    BITMAP bm{};
    const bool ok = GetObjectW(bmp, sizeof(bm), &bm) != 0 && bm.bmWidth > 0 && bm.bmHeight > 0;
    if (ok) {
        outW = bm.bmWidth;
        outH = bm.bmHeight;
    }
    DeleteBitmapHandle(bmp);
    return ok;
}

HBITMAP CaptureScreenOrFrozenRegion(int x1, int y1, int x2, int y2,
    HBITMAP frozenScreen, int frozenVirtX, int frozenVirtY) {
    if (x2 < x1) std::swap(x1, x2);
    if (y2 < y1) std::swap(y1, y2);
    if (x2 <= x1 || y2 <= y1) return nullptr;
    if (frozenScreen) {
        const int cropX = x1 - frozenVirtX;
        const int cropY = y1 - frozenVirtY;
        const int w = x2 - x1;
        const int h = y2 - y1;
        HDC screenDc = GetDC(nullptr);
        HDC memDc = CreateCompatibleDC(screenDc);
        HBITMAP cropBmp = CreateCompatibleBitmap(screenDc, w, h);
        HGDIOBJ oldCrop = SelectObject(memDc, cropBmp);
        HDC srcDc = CreateCompatibleDC(screenDc);
        HGDIOBJ oldSrc = SelectObject(srcDc, frozenScreen);
        BitBlt(memDc, 0, 0, w, h, srcDc, cropX, cropY, SRCCOPY);
        SelectObject(srcDc, oldSrc);
        DeleteDC(srcDc);
        SelectObject(memDc, oldCrop);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
        return cropBmp;
    }
    return CaptureScreenRegion(x1, y1, x2, y2);
}

bool CommitSavedImage(HBITMAP bmp, const std::wstring& target,
    unsigned long long runId,
    std::unordered_map<std::wstring, std::wstring>& imageVars) {
    if (!bmp || target.empty()) return false;
    std::wstring t = target;
    while (!t.empty() && (t.front() == L' ' || t.front() == L'\t')) t.erase(t.begin());
    while (!t.empty() && (t.back() == L' ' || t.back() == L'\t')) t.pop_back();
    if (t.empty()) return false;

    if (LooksLikeFilePath(t)) {
        std::wstring path = t;
        // 无扩展名时默认 .bmp
        const auto slash = path.find_last_of(L"\\/");
        const auto dot = path.find_last_of(L'.');
        if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) {
            path += L".bmp";
        }
        return SaveHBitmapByExtension(bmp, path);
    }

    // 临时图片变量
    const std::wstring path = MakeImageVarTempPath(runId, t);
    EnsureParentDir(path);
    auto it = imageVars.find(t);
    if (it != imageVars.end() && !it->second.empty() && it->second != path) {
        DeleteFileW(it->second.c_str());
    }
    if (!SaveHBitmapByExtension(bmp, path)) return false;
    imageVars[t] = path;
    return true;
}
