#include "window_capture_wgc.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <algorithm>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace wgc = winrt::Windows::Graphics::Capture;
namespace wgdx = winrt::Windows::Graphics::DirectX;
namespace wgd3d = winrt::Windows::Graphics::DirectX::Direct3D11;

namespace windowmode {

namespace {

void EnsureWinRtApartment() {
    // Must only run on the dedicated WGC worker thread — never on the script/UI thread,
    // or VirtualDesktopAccessor COM calls on that thread can access-violate afterward.
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
}

winrt::com_ptr<ID3D11Device> CreateD3DDevice() {
    winrt::com_ptr<ID3D11Device> device;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };
    const HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
        levels, 1, D3D11_SDK_VERSION, device.put(), nullptr, nullptr);
    if (FAILED(hr)) return nullptr;
    return device;
}

wgd3d::IDirect3DDevice CreateWinRtDevice(const winrt::com_ptr<ID3D11Device>& device) {
    winrt::com_ptr<IDXGIDevice> dxgiDevice = device.as<IDXGIDevice>();
    winrt::com_ptr<::IInspectable> inspectable;
    winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put()));
    return inspectable.as<wgd3d::IDirect3DDevice>();
}

wgc::GraphicsCaptureItem CreateCaptureItem(HWND hwnd) {
    auto factory = winrt::get_activation_factory<wgc::GraphicsCaptureItem,
        IGraphicsCaptureItemInterop>();
    wgc::GraphicsCaptureItem item{ nullptr };
    winrt::check_hresult(factory->CreateForWindow(
        hwnd, winrt::guid_of<ABI::Windows::Graphics::Capture::IGraphicsCaptureItem>(),
        reinterpret_cast<void**>(winrt::put_abi(item))));
    return item;
}

HBITMAP CopyTextureToBitmap(ID3D11DeviceContext* context, ID3D11Texture2D* texture,
    UINT width, UINT height) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;

    winrt::com_ptr<ID3D11Device> device;
    texture->GetDevice(device.put());

    winrt::com_ptr<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, staging.put()))) return nullptr;
    context->CopyResource(staging.get(), texture);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped))) return nullptr;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = static_cast<LONG>(width);
    bmi.bmiHeader.biHeight = -static_cast<LONG>(height);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HDC hdc = GetDC(nullptr);
    HBITMAP bmp = CreateDIBSection(hdc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (bmp && bits) {
        const UINT dstPitch = width * 4;
        auto* dst = static_cast<uint8_t*>(bits);
        auto* src = static_cast<const uint8_t*>(mapped.pData);
        for (UINT y = 0; y < height; ++y) {
            memcpy(dst + static_cast<size_t>(y) * dstPitch,
                src + static_cast<size_t>(y) * mapped.RowPitch, dstPitch);
        }
    }
    context->Unmap(staging.get(), 0);
    return bmp;
}

bool IsWgcCaptureAvailableOnThread() {
    try {
        EnsureWinRtApartment();
        return wgc::GraphicsCaptureSession::IsSupported();
    } catch (...) {
        return false;
    }
}

HBITMAP CaptureWindowWgcOnThread(HWND hwnd, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    if (!hwnd || !IsWindow(hwnd)) return nullptr;
    if (IsIconic(hwnd)) return nullptr;
    if (!IsWgcCaptureAvailableOnThread()) return nullptr;

    RECT clientRc{};
    GetClientRect(hwnd, &clientRc);
    const int expectW = std::max(1, static_cast<int>(clientRc.right - clientRc.left));
    const int expectH = std::max(1, static_cast<int>(clientRc.bottom - clientRc.top));

    try {
        EnsureWinRtApartment();

        const winrt::com_ptr<ID3D11Device> d3d = CreateD3DDevice();
        if (!d3d) return nullptr;

        winrt::com_ptr<ID3D11DeviceContext> context;
        d3d->GetImmediateContext(context.put());

        const wgc::GraphicsCaptureItem item = CreateCaptureItem(hwnd);
        auto size = item.Size();
        if (size.Width <= 0 || size.Height <= 0) return nullptr;

        const wgd3d::IDirect3DDevice device = CreateWinRtDevice(d3d);
        auto framePool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(
            device, wgdx::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        auto session = framePool.CreateCaptureSession(item);
        session.IsCursorCaptureEnabled(false);
        try {
            session.IsBorderRequired(false);
        } catch (...) {
        }
        session.StartCapture();

        wgc::Direct3D11CaptureFrame frame{ nullptr };
        for (int attempt = 0; attempt < 60; ++attempt) {
            frame = framePool.TryGetNextFrame();
            if (frame) {
                size = frame.ContentSize();
                if (size.Width >= static_cast<int32_t>(expectW / 2)
                    && size.Height >= static_cast<int32_t>(std::min(expectH / 2, 32))) {
                    break;
                }
                frame = nullptr;
            }
            if (!frame) Sleep(25);
        }

        session.Close();
        framePool.Close();
        if (!frame) return nullptr;

        const auto surface = frame.Surface();
        auto access = surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        winrt::com_ptr<ID3D11Texture2D> texture;
        winrt::check_hresult(access->GetInterface(
            winrt::guid_of<ID3D11Texture2D>(), texture.put_void()));

        outW = static_cast<int>(size.Width);
        outH = static_cast<int>(size.Height);
        return CopyTextureToBitmap(context.get(), texture.get(),
            static_cast<UINT>(size.Width), static_cast<UINT>(size.Height));
    } catch (...) {
        return nullptr;
    }
}

template <typename Fn>
auto RunOnWgcWorker(Fn&& fn) -> decltype(fn()) {
    using R = decltype(fn());
    R result{};
    std::thread worker([&]() {
        result = fn();
    });
    worker.join();
    return result;
}

}  // namespace

bool IsWgcCaptureAvailable() {
    static std::once_flag once;
    static std::atomic<bool> available{false};
    std::call_once(once, [] {
        available.store(RunOnWgcWorker([] { return IsWgcCaptureAvailableOnThread(); }),
            std::memory_order_relaxed);
    });
    return available.load(std::memory_order_relaxed);
}

HBITMAP CaptureWindowWgc(HWND hwnd, int& outW, int& outH) {
    outW = 0;
    outH = 0;
    if (!hwnd || !IsWindow(hwnd)) return nullptr;

    struct Result {
        HBITMAP bmp = nullptr;
        int w = 0;
        int h = 0;
    };

    const Result result = RunOnWgcWorker([hwnd] {
        Result local{};
        local.bmp = CaptureWindowWgcOnThread(hwnd, local.w, local.h);
        return local;
    });

    outW = result.w;
    outH = result.h;
    return result.bmp;
}

}  // namespace windowmode
