#include "render_font.h"

#include "render_device.h"

#include <algorithm>
#include <unordered_map>

namespace {

struct FontKey {
    wchar_t face[LF_FACESIZE]{};
    LONG height = 0;
    LONG weight = 0;
    BYTE italic = 0;

    bool operator==(const FontKey& o) const {
        return height == o.height && weight == o.weight && italic == o.italic
            && wcscmp(face, o.face) == 0;
    }
};

struct FontKeyHash {
    size_t operator()(const FontKey& k) const {
        size_t h = std::hash<LONG>{}(k.height);
        h ^= std::hash<LONG>{}(k.weight) << 1;
        h ^= std::hash<BYTE>{}(k.italic) << 2;
        for (const wchar_t* p = k.face; *p; ++p) h ^= std::hash<wchar_t>{}(*p) << 3;
        return h;
    }
};

std::unordered_map<FontKey, IDWriteTextFormat*, FontKeyHash> g_fontCache;
IDWriteTextFormat* g_defaultFormat = nullptr;

FontKey KeyFromFont(HFONT font) {
    FontKey key{};
    LOGFONTW lf{};
    if (font && GetObjectW(font, sizeof(lf), &lf)) {
        wcsncpy_s(key.face, lf.lfFaceName, _TRUNCATE);
        key.height = lf.lfHeight;
        key.weight = lf.lfWeight;
        key.italic = lf.lfItalic;
    } else {
        wcsncpy_s(key.face, L"Segoe UI", _TRUNCATE);
        key.height = -14;
        key.weight = FW_NORMAL;
    }
    return key;
}

IDWriteTextFormat* CreateFormatFromKey(const FontKey& key) {
    IDWriteFactory* factory = GetDWriteFactory();
    if (!factory) return nullptr;

    const float size = std::max(8.0f, static_cast<float>(abs(key.height)));
    IDWriteTextFormat* format = nullptr;
    if (FAILED(factory->CreateTextFormat(
            key.face[0] ? key.face : L"Segoe UI",
            nullptr,
            static_cast<DWRITE_FONT_WEIGHT>(key.weight),
            key.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            size,
            L"zh-cn",
            &format))) {
        return nullptr;
    }
    return format;
}

}  // namespace

IDWriteTextFormat* AcquireDWriteTextFormat(HFONT font) {
    IDWriteFactory* factory = GetDWriteFactory();
    if (!factory) return nullptr;

    if (!font) {
        if (!g_defaultFormat) {
            FontKey key{};
            wcsncpy_s(key.face, L"Segoe UI", _TRUNCATE);
            key.height = -14;
            key.weight = FW_NORMAL;
            g_defaultFormat = CreateFormatFromKey(key);
        }
        return g_defaultFormat;
    }

    const FontKey key = KeyFromFont(font);
    auto it = g_fontCache.find(key);
    if (it != g_fontCache.end()) return it->second;

    IDWriteTextFormat* format = CreateFormatFromKey(key);
    if (format) g_fontCache[key] = format;
    return format;
}

void ClearDWriteFontCache() {
    for (auto& kv : g_fontCache) {
        if (kv.second) kv.second->Release();
    }
    g_fontCache.clear();
    if (g_defaultFormat) {
        g_defaultFormat->Release();
        g_defaultFormat = nullptr;
    }
}
