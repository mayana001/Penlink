#pragma once

#include <windows.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <roapi.h>
#include <winstring.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <wrl/client.h>

// WinRT initialization is per thread. The capture objects use a free-threaded frame pool.
class WgcApartment {
    HRESULT status_ = RoInitialize(RO_INIT_MULTITHREADED);

public:
    WgcApartment() = default;
    WgcApartment(const WgcApartment&) = delete;
    WgcApartment& operator=(const WgcApartment&) = delete;
    ~WgcApartment() { if (SUCCEEDED(status_)) RoUninitialize(); }

    HRESULT result() const { return status_ == RPC_E_CHANGED_MODE ? S_OK : status_; }
};

class WgcCapture {
    template<class T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    ComPtr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFramePool> pool_;
    ComPtr<ABI::Windows::Graphics::Capture::IGraphicsCaptureSession> session_;
    ComPtr<ABI::Windows::Graphics::Capture::IDirect3D11CaptureFrame> frame_;
    ABI::Windows::Graphics::SizeInt32 size_{};
    HMONITOR monitor_ = nullptr;
    RECT monitorRect_{};
    ULONGLONG lastMonitorCheck_ = 0;

    template<class T, size_t N>
    static HRESULT factory(const wchar_t (&name)[N], ComPtr<T>& result) {
        HSTRING_HEADER header{};
        HSTRING className = nullptr;
        HRESULT hr = WindowsCreateStringReference(name, static_cast<UINT32>(N - 1), &header, &className);
        return FAILED(hr) ? hr : RoGetActivationFactory(className, IID_PPV_ARGS(result.GetAddressOf()));
    }

    template<class T>
    static void close(ComPtr<T>& object) {
        ComPtr<ABI::Windows::Foundation::IClosable> closable;
        if (object && SUCCEEDED(object.As(&closable))) closable->Close();
        object.Reset();
    }

public:
    ~WgcCapture() { reset(); }
    WgcCapture() = default;
    WgcCapture(const WgcCapture&) = delete;
    WgcCapture& operator=(const WgcCapture&) = delete;

    HRESULT init(HMONITOR monitor, ID3D11Device* device, int width, int height) {
        using namespace ABI::Windows::Graphics;
        reset();
        MONITORINFO monitorInfo{sizeof(MONITORINFO)};
        if (!GetMonitorInfoW(monitor, &monitorInfo)) return DXGI_ERROR_ACCESS_LOST;

        ComPtr<Capture::IGraphicsCaptureSessionStatics> support;
        HRESULT hr = factory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession, support);
        if (FAILED(hr)) return hr;
        boolean supported = false;
        hr = support->IsSupported(&supported);
        if (FAILED(hr)) return hr;
        if (!supported) return DXGI_ERROR_UNSUPPORTED;

        ComPtr<IGraphicsCaptureItemInterop> interop;
        hr = factory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem, interop);
        if (FAILED(hr)) return hr;
        ComPtr<Capture::IGraphicsCaptureItem> item;
        hr = interop->CreateForMonitor(monitor, IID_PPV_ARGS(item.GetAddressOf()));
        if (FAILED(hr)) return hr;
        hr = item->get_Size(&size_);
        if (FAILED(hr)) return hr;
        if (size_.Width != width || size_.Height != height) return DXGI_ERROR_ACCESS_LOST;

        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        ComPtr<ID3D11Multithread> multithread;
        if (SUCCEEDED(context.As(&multithread))) multithread->SetMultithreadProtected(TRUE);

        ComPtr<IDXGIDevice> dxgiDevice;
        hr = device->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()));
        if (FAILED(hr)) return hr;
        ComPtr<IInspectable> wrappedDevice;
        hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), wrappedDevice.GetAddressOf());
        if (FAILED(hr)) return hr;
        ComPtr<DirectX::Direct3D11::IDirect3DDevice> deviceWinRT;
        hr = wrappedDevice.As(&deviceWinRT);
        if (FAILED(hr)) return hr;

        ComPtr<Capture::IDirect3D11CaptureFramePoolStatics2> poolFactory;
        hr = factory(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool, poolFactory);
        if (FAILED(hr)) return hr;
        hr = poolFactory->CreateFreeThreaded(deviceWinRT.Get(), DirectX::DirectXPixelFormat_B8G8R8A8UIntNormalized,
                                             2, size_, pool_.GetAddressOf());
        if (FAILED(hr)) return hr;
        hr = pool_->CreateCaptureSession(item.Get(), session_.GetAddressOf());
        if (FAILED(hr)) {
            reset();
            return hr;
        }
        monitor_ = monitor;
        monitorRect_ = monitorInfo.rcMonitor;
        return S_OK;
    }

    bool ready() const { return session_ != nullptr; }

    HRESULT start() { return session_ ? session_->StartCapture() : E_UNEXPECTED; }

    // Keep the WinRT frame alive until the caller has copied its surface.
    HRESULT acquire(ComPtr<ID3D11Texture2D>& texture) {
        using namespace ABI::Windows::Graphics;
        releaseFrame();
        texture.Reset();
        if (!pool_) return DXGI_ERROR_ACCESS_LOST;

        const ULONGLONG now = GetTickCount64();
        if (now - lastMonitorCheck_ >= 250) {
            lastMonitorCheck_ = now;
            MONITORINFO info{sizeof(MONITORINFO)};
            if (!GetMonitorInfoW(monitor_, &info) || !EqualRect(&info.rcMonitor, &monitorRect_))
                return DXGI_ERROR_ACCESS_LOST;
        }

        HRESULT hr = pool_->TryGetNextFrame(frame_.GetAddressOf());
        if (FAILED(hr)) return hr;
        if (!frame_) return DXGI_ERROR_WAIT_TIMEOUT;
        SizeInt32 content{};
        hr = frame_->get_ContentSize(&content);
        if (FAILED(hr)) return hr;
        if (content.Width != size_.Width || content.Height != size_.Height)
            return DXGI_ERROR_ACCESS_LOST;
        ComPtr<DirectX::Direct3D11::IDirect3DSurface> surface;
        hr = frame_->get_Surface(surface.GetAddressOf());
        if (FAILED(hr)) return hr;
        ComPtr<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
        hr = surface.As(&access);
        if (FAILED(hr)) return hr;
        hr = access->GetInterface(IID_PPV_ARGS(texture.GetAddressOf()));
        if (FAILED(hr)) return hr;
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Width != static_cast<UINT>(size_.Width) || desc.Height != static_cast<UINT>(size_.Height) ||
            desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
            return DXGI_ERROR_ACCESS_LOST;
        return S_OK;
    }

    void releaseFrame() { close(frame_); }

    void reset() {
        releaseFrame();
        close(session_);
        close(pool_);
        monitor_ = nullptr;
        lastMonitorCheck_ = 0;
    }
};
