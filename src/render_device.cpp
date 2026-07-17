// ── Direct2D / DirectWrite 设备生命周期 ─────────────────────────
#include "render_device.h"

#include "render_font.h"

#include <objbase.h>

namespace {

ID2D1Factory* g_d2dFactory = nullptr;
IDWriteFactory* g_dwriteFactory = nullptr;
RenderBackend g_preferredBackend = RenderBackend::Gdi;
bool g_comInitialized = false;

}  // namespace

bool InitRenderDevice() {
    if (g_d2dFactory) return true;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (SUCCEEDED(hr)) {
        g_comInitialized = true;
    } else if (hr != RPC_E_CHANGED_MODE) {
        return false;
    }

    D2D1_FACTORY_OPTIONS options{};
#if defined(_DEBUG)
    options.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
        &options, reinterpret_cast<void**>(&g_d2dFactory));
    if (FAILED(hr)) {
        g_d2dFactory = nullptr;
        g_preferredBackend = RenderBackend::Gdi;
        return false;
    }

    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&g_dwriteFactory));
    if (FAILED(hr)) {
        g_dwriteFactory = nullptr;
    }

    return true;
}

void ShutdownRenderDevice() {
    ClearDWriteFontCache();
    if (g_dwriteFactory) {
        g_dwriteFactory->Release();
        g_dwriteFactory = nullptr;
    }
    if (g_d2dFactory) {
        g_d2dFactory->Release();
        g_d2dFactory = nullptr;
    }
    if (g_comInitialized) {
        CoUninitialize();
        g_comInitialized = false;
    }
}

bool IsDirect2DAvailable() {
    return g_d2dFactory != nullptr;
}

bool IsDirectWriteAvailable() {
    return g_dwriteFactory != nullptr;
}

RenderBackend GetPreferredRenderBackend() {
    if (g_preferredBackend == RenderBackend::Direct2D
        && (!IsDirect2DAvailable() || !IsDirectWriteAvailable())) {
        return RenderBackend::Gdi;
    }
    return g_preferredBackend;
}

void SetPreferredRenderBackend(RenderBackend backend) {
    g_preferredBackend = backend;
}

ID2D1Factory* GetD2DFactory() {
    return g_d2dFactory;
}

IDWriteFactory* GetDWriteFactory() {
    return g_dwriteFactory;
}
