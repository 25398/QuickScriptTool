#include "color_match.h"

#include "image_match.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <vector>

namespace {

int ClampByte(int v) {
    return std::max(0, std::min(255, v));
}

bool ParseHex2(wchar_t a, wchar_t b, int& out) {
    auto nibble = [](wchar_t c) -> int {
        if (c >= L'0' && c <= L'9') return c - L'0';
        c = static_cast<wchar_t>(towupper(c));
        if (c >= L'A' && c <= L'F') return 10 + (c - L'A');
        return -1;
    };
    const int hi = nibble(a);
    const int lo = nibble(b);
    if (hi < 0 || lo < 0) return false;
    out = (hi << 4) | lo;
    return true;
}

}  // namespace

bool TryParseColorSpec(const std::wstring& text, int& outR, int& outG, int& outB) {
    outR = outG = outB = 0;
    std::wstring t;
    t.reserve(text.size());
    for (wchar_t c : text) {
        if (c == L' ' || c == L'\t' || c == L'\r' || c == L'\n') continue;
        t.push_back(c);
    }
    if (t.empty()) return false;

    if (t[0] == L'#') t.erase(t.begin());
    if (t.size() == 6) {
        int r = 0, g = 0, b = 0;
        if (!ParseHex2(t[0], t[1], r) || !ParseHex2(t[2], t[3], g) || !ParseHex2(t[4], t[5], b))
            return false;
        outR = r;
        outG = g;
        outB = b;
        return true;
    }

    // r,g,b
    int vals[3] = {};
    int idx = 0;
    std::wstring cur;
    for (size_t i = 0; i <= t.size() && idx < 3; ++i) {
        const wchar_t c = (i < t.size()) ? t[i] : L',';
        if (c == L',' || c == L';' || c == L'|' || i == t.size()) {
            if (!cur.empty()) {
                vals[idx++] = ClampByte(_wtoi(cur.c_str()));
                cur.clear();
            }
            continue;
        }
        cur.push_back(c);
    }
    if (idx < 3) return false;
    outR = vals[0];
    outG = vals[1];
    outB = vals[2];
    return true;
}

std::wstring FormatColorHex(int r, int g, int b) {
    wchar_t buf[16];
    swprintf_s(buf, L"#%02X%02X%02X", ClampByte(r), ClampByte(g), ClampByte(b));
    return buf;
}

int ColorChannelDistance(int r1, int g1, int b1, int r2, int g2, int b2) {
    return std::max({ std::abs(r1 - r2), std::abs(g1 - g2), std::abs(b1 - b2) });
}

bool ColorsMatch(int r1, int g1, int b1, int r2, int g2, int b2, int tolerance) {
    return ColorChannelDistance(r1, g1, b1, r2, g2, b2) <= std::max(0, tolerance);
}

bool GetScreenPixelRgb(int x, int y, int& outR, int& outG, int& outB,
    HBITMAP frozenScreen, int frozenVirtX, int frozenVirtY) {
    outR = outG = outB = 0;
    COLORREF c = RGB(0, 0, 0);
    if (frozenScreen) {
        HDC screenDc = GetDC(nullptr);
        HDC memDc = CreateCompatibleDC(screenDc);
        HGDIOBJ old = SelectObject(memDc, frozenScreen);
        c = GetPixel(memDc, x - frozenVirtX, y - frozenVirtY);
        SelectObject(memDc, old);
        DeleteDC(memDc);
        ReleaseDC(nullptr, screenDc);
    } else {
        HDC hdc = GetDC(nullptr);
        if (!hdc) return false;
        c = GetPixel(hdc, x, y);
        ReleaseDC(nullptr, hdc);
    }
    if (c == CLR_INVALID) return false;
    outR = GetRValue(c);
    outG = GetGValue(c);
    outB = GetBValue(c);
    return true;
}

bool SampleClickColorGrid(int cx, int cy, int* outR, int* outG, int* outB,
    HBITMAP frozenScreen, int frozenVirtX, int frozenVirtY) {
    if (!outR || !outG || !outB) return false;
    int i = 0;
    const int step = 8;
    for (int dy = -2; dy <= 2; ++dy) {
        for (int dx = -2; dx <= 2; ++dx) {
            if (!GetScreenPixelRgb(cx + dx * step, cy + dy * step,
                    outR[i], outG[i], outB[i],
                    frozenScreen, frozenVirtX, frozenVirtY)) {
                return false;
            }
            ++i;
        }
    }
    return i == kClickColorGridN;
}

bool ClickColorGridChanged(
    const int* beforeR, const int* beforeG, const int* beforeB,
    const int* afterR, const int* afterG, const int* afterB,
    int tolerance, int minChanged) {
    if (!beforeR || !beforeG || !beforeB || !afterR || !afterG || !afterB)
        return false;
    int changed = 0;
    for (int i = 0; i < kClickColorGridN; ++i) {
        if (!ColorsMatch(beforeR[i], beforeG[i], beforeB[i],
                afterR[i], afterG[i], afterB[i], tolerance)) {
            ++changed;
        }
    }
    return changed >= std::max(1, minChanged);
}

namespace {

struct ColorScanBitmap {
    int width = 0;
    int height = 0;
    int originX = 0;  // 位图 (0,0) 对应的屏幕坐标
    int originY = 0;
    std::vector<uint8_t> bgra;  // top-down 32bpp BGRA
    HBITMAP owned = nullptr;    // 非空则析构时 DeleteObject
};

void ReleaseColorScanBitmap(ColorScanBitmap& s) {
    if (s.owned) {
        DeleteBitmapHandle(s.owned);
        s.owned = nullptr;
    }
    s.bgra.clear();
    s.width = s.height = 0;
}

bool ReadHbitmapBgra(HBITMAP bitmap, int& outW, int& outH, std::vector<uint8_t>& outBgra) {
    if (!bitmap) return false;
    BITMAP bm{};
    if (!GetObjectW(bitmap, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0)
        return false;
    outW = bm.bmWidth;
    outH = bm.bmHeight;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = outW;
    bi.bmiHeader.biHeight = -outH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    outBgra.resize(static_cast<size_t>(outW) * static_cast<size_t>(outH) * 4);
    HDC dc = GetDC(nullptr);
    if (!dc) return false;
    const int lines = GetDIBits(dc, bitmap, 0, outH, outBgra.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, dc);
    return lines > 0;
}

bool PrepareColorScanBitmap(
    int left, int top, int right, int bottom,
    HBITMAP frozenScreen, int frozenVirtX, int frozenVirtY,
    ColorScanBitmap& out) {
    ReleaseColorScanBitmap(out);
    if (frozenScreen) {
        if (!ReadHbitmapBgra(frozenScreen, out.width, out.height, out.bgra))
            return false;
        out.originX = frozenVirtX;
        out.originY = frozenVirtY;
        return true;
    }
    out.owned = CaptureScreenRegion(left, top, right, bottom);
    if (!out.owned) return false;
    if (!ReadHbitmapBgra(out.owned, out.width, out.height, out.bgra)) {
        ReleaseColorScanBitmap(out);
        return false;
    }
    out.originX = left;
    out.originY = top;
    return true;
}

}  // namespace

ColorMatchHit FindColorInScreenRegion(
    int searchX1, int searchY1, int searchX2, int searchY2,
    int targetR, int targetG, int targetB, int tolerance,
    HBITMAP frozenScreen, int frozenVirtX, int frozenVirtY,
    int stepPx,
    const std::atomic_bool* cancelFlag) {
    ColorMatchHit hit{};
    const int left = std::min(searchX1, searchX2);
    const int top = std::min(searchY1, searchY2);
    const int right = std::max(searchX1, searchX2);
    const int bottom = std::max(searchY1, searchY2);
    if (right <= left || bottom <= top) return hit;
    if (cancelFlag && cancelFlag->load(std::memory_order_relaxed)) return hit;

    ColorScanBitmap scan{};
    if (!PrepareColorScanBitmap(left, top, right, bottom,
            frozenScreen, frozenVirtX, frozenVirtY, scan)) {
        return hit;
    }

    const int step = std::max(1, stepPx);
    const int tol = std::max(0, tolerance);
    int bestDist = 256;
    int bestX = left;
    int bestY = top;
    int bestR = 0, bestG = 0, bestB = 0;
    bool any = false;

    const int localL = std::max(0, left - scan.originX);
    const int localT = std::max(0, top - scan.originY);
    const int localR = std::min(scan.width, right - scan.originX);
    const int localB = std::min(scan.height, bottom - scan.originY);

    for (int ly = localT; ly < localB; ly += step) {
        if (cancelFlag && cancelFlag->load(std::memory_order_relaxed)) {
            ReleaseColorScanBitmap(scan);
            return ColorMatchHit{};
        }
        const size_t row = static_cast<size_t>(ly) * static_cast<size_t>(scan.width) * 4;
        for (int lx = localL; lx < localR; lx += step) {
            const size_t i = row + static_cast<size_t>(lx) * 4;
            const int b = scan.bgra[i];
            const int g = scan.bgra[i + 1];
            const int r = scan.bgra[i + 2];
            const int d = ColorChannelDistance(r, g, b, targetR, targetG, targetB);
            if (d < bestDist) {
                bestDist = d;
                bestX = scan.originX + lx;
                bestY = scan.originY + ly;
                bestR = r;
                bestG = g;
                bestB = b;
                any = true;
                if (d == 0) break;
            }
        }
        if (bestDist == 0) break;
        // 已找到足够近的匹配时可提前结束（大区域不必扫完）
        if (any && bestDist <= tol && bestDist <= 8) break;
    }

    ReleaseColorScanBitmap(scan);
    if (!any || bestDist > tol) return hit;
    hit.found = true;
    hit.x = bestX;
    hit.y = bestY;
    hit.r = bestR;
    hit.g = bestG;
    hit.b = bestB;
    hit.distance = bestDist;
    return hit;
}

bool MatchColorAtScreenPoint(int x, int y,
    int targetR, int targetG, int targetB, int tolerance,
    int* outR, int* outG, int* outB, int* outDist,
    HBITMAP frozenScreen, int frozenVirtX, int frozenVirtY) {
    int r = 0, g = 0, b = 0;
    if (!GetScreenPixelRgb(x, y, r, g, b, frozenScreen, frozenVirtX, frozenVirtY))
        return false;
    const int d = ColorChannelDistance(r, g, b, targetR, targetG, targetB);
    if (outR) *outR = r;
    if (outG) *outG = g;
    if (outB) *outB = b;
    if (outDist) *outDist = d;
    return d <= std::max(0, tolerance);
}
