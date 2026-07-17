#include "window_capture.h"
#include "window_capture_wgc.h"
#include "window_target.h"

#include <algorithm>
#include <vector>

#ifndef WM_PRINT
#define WM_PRINT 0x0317
#endif
#ifndef PRF_CHECKVISIBLE
#define PRF_CHECKVISIBLE 0x00000001L
#define PRF_NONCLIENT  0x00000002L
#define PRF_CLIENT     0x00000004L
#define PRF_CHILDREN   0x00000010L
#endif

namespace windowmode {

namespace {

struct RawBitmap {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> pixels;
};

bool ReadBitmapPixels(HBITMAP bitmap, RawBitmap& out) {
    if (!bitmap) return false;

    BITMAP bm{};
    if (!GetObject(bitmap, sizeof(bm), &bm)) return false;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = bm.bmWidth;
    bi.bmiHeader.biHeight = -bm.bmHeight;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    out.width = bm.bmWidth;
    out.height = bm.bmHeight;
    out.pixels.resize(static_cast<size_t>(out.width) * out.height * 4);

    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    const int lines = GetDIBits(dc, bitmap, 0, out.height, out.pixels.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return lines == out.height;
}

}  // namespace

HWND ResolveWgcRoot(HWND hwnd) {
    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root ? root : hwnd;
}

HBITMAP CropClientFromWindowBitmap(HWND hwnd, HBITMAP full, int fullW, int fullH) {
    if (!hwnd || !full || fullW <= 0 || fullH <= 0) return full;

    RECT windowRc{};
    RECT clientRc{};
    if (!GetWindowRect(hwnd, &windowRc) || !GetClientRect(hwnd, &clientRc)) return full;

    const int clientW = std::max(0, static_cast<int>(clientRc.right - clientRc.left));
    const int clientH = std::max(0, static_cast<int>(clientRc.bottom - clientRc.top));
    if (clientW <= 0 || clientH <= 0) return full;

    POINT clientOrigin{0, 0};
    ClientToScreen(hwnd, &clientOrigin);
    const int cropX = clientOrigin.x - windowRc.left;
    const int cropY = clientOrigin.y - windowRc.top;
    if (cropX <= 0 && cropY <= 0
        && clientW >= fullW && clientH >= fullH) {
        return full;
    }

    const int L = std::clamp(cropX, 0, fullW - 1);
    const int T = std::clamp(cropY, 0, fullH - 1);
    const int R = std::clamp(L + clientW, L + 1, fullW);
    const int B = std::clamp(T + clientH, T + 1, fullH);
    const int w = std::max(1, R - L);
    const int h = std::max(1, B - T);

    HDC screenDc = GetDC(nullptr);
    HDC srcDc = CreateCompatibleDC(screenDc);
    HDC dstDc = CreateCompatibleDC(screenDc);
    HBITMAP cropped = CreateCompatibleBitmap(screenDc, w, h);
    HGDIOBJ oldSrc = SelectObject(srcDc, full);
    HGDIOBJ oldDst = SelectObject(dstDc, cropped);
    BitBlt(dstDc, 0, 0, w, h, srcDc, L, T, SRCCOPY);
    SelectObject(srcDc, oldSrc);
    SelectObject(dstDc, oldDst);
    DeleteDC(srcDc);
    DeleteDC(dstDc);
    ReleaseDC(nullptr, screenDc);
    DeleteObject(full);
    return cropped;
}

HBITMAP CropChildFromRootBitmap(HWND root, HWND child, HBITMAP full, int fullW, int fullH) {
    if (!root || !child || root == child || !full) return full;

    RECT childRc{};
    if (!GetClientRect(child, &childRc)) return full;
    const int childW = std::max(0, static_cast<int>(childRc.right - childRc.left));
    const int childH = std::max(0, static_cast<int>(childRc.bottom - childRc.top));
    if (childW <= 0 || childH <= 0) return full;

    RECT mapped = childRc;
    SetLastError(0);
    MapWindowPoints(child, root, reinterpret_cast<LPPOINT>(&mapped), 2);
    // MapWindowPoints 成功时也可能返回 0（零偏移）；须用 GetLastError 判失败
    if (GetLastError() != 0) return full;

    const int L = std::clamp(static_cast<int>(mapped.left), 0, fullW - 1);
    const int T = std::clamp(static_cast<int>(mapped.top), 0, fullH - 1);
    const int R = std::clamp(static_cast<int>(mapped.right), L + 1, fullW);
    const int B = std::clamp(static_cast<int>(mapped.bottom), T + 1, fullH);
    const int w = std::max(1, R - L);
    const int h = std::max(1, B - T);

    HDC screenDc = GetDC(nullptr);
    HDC srcDc = CreateCompatibleDC(screenDc);
    HDC dstDc = CreateCompatibleDC(screenDc);
    HBITMAP cropped = CreateCompatibleBitmap(screenDc, w, h);
    HGDIOBJ oldSrc = SelectObject(srcDc, full);
    HGDIOBJ oldDst = SelectObject(dstDc, cropped);
    BitBlt(dstDc, 0, 0, w, h, srcDc, L, T, SRCCOPY);
    SelectObject(srcDc, oldSrc);
    SelectObject(dstDc, oldDst);
    DeleteDC(srcDc);
    DeleteDC(dstDc);
    ReleaseDC(nullptr, screenDc);
    DeleteObject(full);
    return cropped;
}

WindowCaptureResult CaptureWindowClientGdi(HWND captureHwnd) {
    WindowCaptureResult result{};
    if (!captureHwnd || !IsWindow(captureHwnd)) return result;

    RECT clientRc{};
    if (!GetClientRect(captureHwnd, &clientRc)) return result;

    const int w = std::max(1, static_cast<int>(clientRc.right - clientRc.left));
    const int h = std::max(1, static_cast<int>(clientRc.bottom - clientRc.top));

    POINT origin{0, 0};
    ClientToScreen(captureHwnd, &origin);

    HDC screenDc = GetDC(nullptr);
    HDC memDc = CreateCompatibleDC(screenDc);
    HBITMAP bmp = CreateCompatibleBitmap(screenDc, w, h);
    HGDIOBJ oldBmp = SelectObject(memDc, bmp);

    BOOL printed = PrintWindow(captureHwnd, memDc, PW_RENDERFULLCONTENT);
    // PrintWindow(PW_RENDERFULLCONTENT) 成功时不要再 WM_PRINT：
    // Chrome/Electron 的 WM_PRINT 常会覆盖成错误/空内容。
    if (!printed) {
        SendMessageW(captureHwnd, WM_PRINT, reinterpret_cast<WPARAM>(memDc),
            PRF_CLIENT | PRF_CHILDREN | PRF_CHECKVISIBLE);
    }

    HWND root = GetAncestor(captureHwnd, GA_ROOT);
    if (root && root != captureHwnd && IsIconic(root)) {
        RedrawWindow(captureHwnd, nullptr, nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_ERASE);
        printed = PrintWindow(captureHwnd, memDc, PW_RENDERFULLCONTENT);
        if (!printed) {
            SendMessageW(captureHwnd, WM_PRINT, reinterpret_cast<WPARAM>(memDc),
                PRF_CLIENT | PRF_CHILDREN | PRF_CHECKVISIBLE);
        }
    }

    if (!printed) {
        printed = PrintWindow(captureHwnd, memDc, PW_RENDERFULLCONTENT);
    }
    if (!printed) {
        HDC wndDc = GetDC(captureHwnd);
        if (wndDc) {
            BitBlt(memDc, 0, 0, w, h, wndDc, 0, 0, SRCCOPY);
            ReleaseDC(captureHwnd, wndDc);
        }
    }

    SelectObject(memDc, oldBmp);
    DeleteDC(memDc);
    ReleaseDC(nullptr, screenDc);

    result.bitmap = bmp;
    result.x = origin.x;
    result.y = origin.y;
    result.w = w;
    result.h = h;
    result.fromPrintWindow = printed == TRUE;
    return result;
}

WindowCaptureResult CaptureWindowClientWgc(HWND hwnd, HWND captureHwnd) {
    WindowCaptureResult result{};
    if (!captureHwnd || !IsWindow(captureHwnd)) return result;

    const HWND wgcRoot = ResolveWgcRoot(captureHwnd);
    int wgcW = 0;
    int wgcH = 0;
    HBITMAP wgcBmp = CaptureWindowWgc(wgcRoot, wgcW, wgcH);
    if (!wgcBmp) return result;

    if (captureHwnd != wgcRoot) {
        wgcBmp = CropChildFromRootBitmap(wgcRoot, captureHwnd, wgcBmp, wgcW, wgcH);
    } else {
        wgcBmp = CropClientFromWindowBitmap(captureHwnd, wgcBmp, wgcW, wgcH);
    }
    if (!wgcBmp) return result;

    BITMAP bm{};
    if (GetObject(wgcBmp, sizeof(bm), &bm)) {
        result.w = bm.bmWidth;
        result.h = bm.bmHeight;
    } else {
        result.w = wgcW;
        result.h = wgcH;
    }

    POINT origin{0, 0};
    ClientToScreen(captureHwnd, &origin);
    result.bitmap = wgcBmp;
    result.x = origin.x;
    result.y = origin.y;
    result.fromPrintWindow = false;
    (void)hwnd;
    return result;
}

WindowCaptureResult CaptureWindowClient(HWND hwnd) {
    if (!hwnd || !IsWindow(hwnd)) return {};

    HWND captureHwnd = hwnd;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    const HWND wgcRoot = ResolveWgcRoot(hwnd);
    const bool offUserDesktop = !IsWindowOnUserCurrentDesktop(wgcRoot);

    if (root && root != hwnd) {
        RECT childRc{};
        const bool childEmpty = !GetClientRect(hwnd, &childRc)
            || (childRc.right - childRc.left) <= 0
            || (childRc.bottom - childRc.top) <= 0;
        // 非当前虚拟桌面且父窗口最小化时，仍对子控件截图（PrintWindow）。
        if (childEmpty || (IsIconic(root) && !offUserDesktop)) {
            captureHwnd = root;
        }
    } else if (IsIconic(hwnd) && !offUserDesktop) {
        captureHwnd = hwnd;
    }

    // 宏桌面最小化 + 子控件绑定：先子窗口 PrintWindow，空白再根窗口裁剪。
    if (offUserDesktop && root && root != hwnd && IsIconic(root)) {
        WindowCaptureResult childCap = CaptureWindowClientGdi(hwnd);
        if (childCap.bitmap && !IsCaptureLikelyBlank(childCap.bitmap)) {
            return childCap;
        }
        if (childCap.bitmap) {
            DeleteObject(childCap.bitmap);
        }

        WindowCaptureResult rootCap = CaptureWindowClientGdi(root);
        if (rootCap.bitmap) {
            rootCap.bitmap = CropChildFromRootBitmap(root, hwnd, rootCap.bitmap,
                rootCap.w, rootCap.h);
            if (rootCap.bitmap && !IsCaptureLikelyBlank(rootCap.bitmap)) {
                return rootCap;
            }
            if (rootCap.bitmap) {
                DeleteObject(rootCap.bitmap);
                rootCap = {};
            }
        }
    }

    const HWND wgcRootCapture = ResolveWgcRoot(captureHwnd);
    const bool offUserDesktopCapture = !IsWindowOnUserCurrentDesktop(wgcRootCapture);

    // 非当前虚拟桌面：仅 GDI PrintWindow（WGC StartCapture 会切到目标虚拟桌面）。
    if (offUserDesktopCapture) {
        WindowCaptureResult gdi = CaptureWindowClientGdi(captureHwnd);
        if (gdi.bitmap && !IsCaptureLikelyBlank(gdi.bitmap)) return gdi;
        if (gdi.bitmap) {
            DeleteObject(gdi.bitmap);
        }
        return {};
    }

    HWND fg = GetForegroundWindow();
    HWND fgRoot = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
    const bool targetIsForeground = fgRoot && (fgRoot == wgcRootCapture);
    const bool rootVisible = !IsIconic(wgcRootCapture);

    // 目标不在前台（后台置底）：优先 GDI，避免 WGC 边框/焦点扰动。
    if (!targetIsForeground) {
        WindowCaptureResult gdi = CaptureWindowClientGdi(captureHwnd);
        if (gdi.bitmap && !IsCaptureLikelyBlank(gdi.bitmap)) return gdi;
        if (gdi.bitmap) {
            DeleteObject(gdi.bitmap);
        }
    }

    if (IsWgcCaptureAvailable() && rootVisible) {
        WindowCaptureResult wgc = CaptureWindowClientWgc(hwnd, captureHwnd);
        if (wgc.bitmap && !IsCaptureLikelyBlank(wgc.bitmap)) return wgc;
        if (wgc.bitmap) {
            DeleteObject(wgc.bitmap);
        }
    }

    WindowCaptureResult gdi = CaptureWindowClientGdi(captureHwnd);
    if (gdi.bitmap && !IsCaptureLikelyBlank(gdi.bitmap)) return gdi;
    if (gdi.bitmap) {
        DeleteObject(gdi.bitmap);
        gdi = {};
    }

    if (IsWgcCaptureAvailable()) {
        WindowCaptureResult wgc = CaptureWindowClientWgc(hwnd, captureHwnd);
        if (wgc.bitmap) return wgc;
    }
    return gdi;
}

WindowCaptureResult CaptureWindowRegion(HWND hwnd, int cx1, int cy1, int cx2, int cy2) {
    WindowCaptureResult full = CaptureWindowClient(hwnd);
    if (!full.bitmap) return full;

    const int L = std::min(cx1, cx2);
    const int T = std::min(cy1, cy2);
    const int R = std::max(cx1, cx2);
    const int B = std::max(cy1, cy2);
    const int w = std::max(1, std::min(R - L, full.w - L));
    const int h = std::max(1, std::min(B - T, full.h - T));

    HDC screenDc = GetDC(nullptr);
    HDC srcDc = CreateCompatibleDC(screenDc);
    HDC dstDc = CreateCompatibleDC(screenDc);
    HBITMAP cropped = CreateCompatibleBitmap(screenDc, w, h);
    HGDIOBJ oldSrc = SelectObject(srcDc, full.bitmap);
    HGDIOBJ oldDst = SelectObject(dstDc, cropped);
    BitBlt(dstDc, 0, 0, w, h, srcDc, L, T, SRCCOPY);
    SelectObject(srcDc, oldSrc);
    SelectObject(dstDc, oldDst);
    DeleteDC(srcDc);
    DeleteDC(dstDc);
    ReleaseDC(nullptr, screenDc);

    DeleteObject(full.bitmap);
    full.bitmap = cropped;
    full.x += L;
    full.y += T;
    full.w = w;
    full.h = h;
    return full;
}

HBITMAP CropBitmapScreenRegion(HBITMAP src, int srcOriginX, int srcOriginY,
    int regionX1, int regionY1, int regionX2, int regionY2) {
    if (!src) return nullptr;
    const int L = std::min(regionX1, regionX2);
    const int T = std::min(regionY1, regionY2);
    const int R = std::max(regionX1, regionX2);
    const int B = std::max(regionY1, regionY2);
    const int w = std::max(1, R - L);
    const int h = std::max(1, B - T);
    const int cropX = L - srcOriginX;
    const int cropY = T - srcOriginY;

    HDC screenDc = GetDC(nullptr);
    HDC srcDc = CreateCompatibleDC(screenDc);
    HDC dstDc = CreateCompatibleDC(screenDc);
    HBITMAP cropped = CreateCompatibleBitmap(screenDc, w, h);
    HGDIOBJ oldSrc = SelectObject(srcDc, src);
    HGDIOBJ oldDst = SelectObject(dstDc, cropped);
    BitBlt(dstDc, 0, 0, w, h, srcDc, cropX, cropY, SRCCOPY);
    SelectObject(srcDc, oldSrc);
    SelectObject(dstDc, oldDst);
    DeleteDC(srcDc);
    DeleteDC(dstDc);
    ReleaseDC(nullptr, screenDc);
    return cropped;
}

HBITMAP CropBitmapClientRegion(HBITMAP src, int cx1, int cy1, int cx2, int cy2) {
    if (!src) return nullptr;
    const int L = std::min(cx1, cx2);
    const int T = std::min(cy1, cy2);
    const int R = std::max(cx1, cx2);
    const int B = std::max(cy1, cy2);
    const int w = std::max(1, R - L);
    const int h = std::max(1, B - T);

    HDC screenDc = GetDC(nullptr);
    HDC srcDc = CreateCompatibleDC(screenDc);
    HDC dstDc = CreateCompatibleDC(screenDc);
    HBITMAP cropped = CreateCompatibleBitmap(screenDc, w, h);
    HGDIOBJ oldSrc = SelectObject(srcDc, src);
    HGDIOBJ oldDst = SelectObject(dstDc, cropped);
    BitBlt(dstDc, 0, 0, w, h, srcDc, L, T, SRCCOPY);
    SelectObject(srcDc, oldSrc);
    SelectObject(dstDc, oldDst);
    DeleteDC(srcDc);
    DeleteDC(dstDc);
    ReleaseDC(nullptr, screenDc);
    return cropped;
}

bool IsCaptureLikelyBlank(HBITMAP bmp) {
    RawBitmap data{};
    if (!ReadBitmapPixels(bmp, data) || data.pixels.empty()) return true;

    uint64_t sum = 0;
    size_t count = 0;
    for (size_t i = 0; i + 3 < data.pixels.size(); i += 4) {
        const uint8_t b = data.pixels[i];
        const uint8_t g = data.pixels[i + 1];
        const uint8_t r = data.pixels[i + 2];
        sum += r + g + b;
        ++count;
    }
    if (count == 0) return true;

    const double avg = static_cast<double>(sum) / static_cast<double>(count * 3);
    return avg < 2.0;
}

}  // namespace windowmode
