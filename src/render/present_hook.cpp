#include "render/present_hook.h"

#include "core/hook.h"
#include "core/log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

namespace mc::render {
namespace {

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

// IDXGISwapChain vtable slots (IUnknown 0-2, IDXGIObject 3-6, IDXGIDeviceSubObject 7, IDXGISwapChain 8-17,
// IDXGISwapChain1 18-28).
constexpr int kPresentIndex = 8;
constexpr int kResizeBuffersIndex = 13;
constexpr int kPresent1Index = 22;

PresentFn _origPresent = nullptr;
Present1Fn _origPresent1 = nullptr;
ResizeBuffersFn _origResizeBuffers = nullptr;
FrameCallback _onFrame = nullptr;
ResizeCallback _onResize = nullptr;
BeforeResizeCallback _onBeforeResize = nullptr;
DeviceLostCallback _onDeviceLost = nullptr;
DWORD _presentThread = 0;  // thread of the first Present (the game's render thread)
IDXGISwapChain* _swapChain = nullptr;
LARGE_INTEGER _freq{};
LARGE_INTEGER _last{};
bool _loggedFirstFrame = false;
thread_local int _depth = 0;  // Present1 may route through Present inside DXGI; tick once

void LogSwapChain(IDXGISwapChain* sc, const char* via) {
    DXGI_SWAP_CHAIN_DESC d{};
    if (SUCCEEDED(sc->GetDesc(&d))) {
        char title[128] = {};
        GetWindowTextA(d.OutputWindow, title, sizeof(title));
        MC_LOG_INFO("render: first frame via %s: swapchain %p %ux%u fmt=%d buffers=%u effect=%d windowed=%d "
                    "hwnd=%p '%s'",
                    via, sc, d.BufferDesc.Width, d.BufferDesc.Height, static_cast<int>(d.BufferDesc.Format),
                    d.BufferCount, static_cast<int>(d.SwapEffect), d.Windowed, d.OutputWindow, title);
    }
}

void OnFrame(IDXGISwapChain* sc, const char* via) {
    _swapChain = sc;
    if (!_loggedFirstFrame) {
        _loggedFirstFrame = true;
        _presentThread = GetCurrentThreadId();
        LogSwapChain(sc, via);
        MC_LOG_INFO("render: Present runs on thread %lu", _presentThread);
    } else if (GetCurrentThreadId() != _presentThread) {
        static bool warned = false;
        if (!warned) {
            warned = true;
            MC_LOG_WARN("render: Present on thread %lu, first frame was on %lu (overlay assumes one render thread)",
                        GetCurrentThreadId(), _presentThread);
        }
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = _last.QuadPart ? static_cast<float>(now.QuadPart - _last.QuadPart) / static_cast<float>(_freq.QuadPart)
                              : 0.0f;
    _last = now;
    if (_onFrame) _onFrame(dt);
}

// The game's device is gone (driver reset, GPU removed): every resource made on it is useless. The game recreates
// its device and swapchain; the overlay rebuilds on the next frame that has one.
void CheckDeviceLost(HRESULT hr) {
    if (hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET) return;
    MC_LOG_WARN("render: Present returned 0x%08lx (device lost); releasing overlay resources",
                static_cast<unsigned long>(hr));
    if (_onDeviceLost) _onDeviceLost();
}

HRESULT STDMETHODCALLTYPE PresentDetour(IDXGISwapChain* sc, UINT syncInterval, UINT flags) {
    if (_depth == 0 && !(flags & DXGI_PRESENT_TEST)) OnFrame(sc, "Present");
    ++_depth;
    HRESULT hr = _origPresent(sc, syncInterval, flags);
    --_depth;
    CheckDeviceLost(hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE Present1Detour(IDXGISwapChain1* sc, UINT syncInterval, UINT flags,
                                         const DXGI_PRESENT_PARAMETERS* params) {
    if (_depth == 0 && !(flags & DXGI_PRESENT_TEST)) OnFrame(sc, "Present1");
    ++_depth;
    HRESULT hr = _origPresent1(sc, syncInterval, flags, params);
    --_depth;
    CheckDeviceLost(hr);
    return hr;
}

HRESULT STDMETHODCALLTYPE ResizeBuffersDetour(IDXGISwapChain* sc, UINT count, UINT width, UINT height,
                                              DXGI_FORMAT format, UINT flags) {
    MC_LOG_INFO("render: ResizeBuffers(%p, count=%u, %ux%u, fmt=%d, flags=0x%x) on thread %lu%s", sc, count, width,
                height, static_cast<int>(format), flags, GetCurrentThreadId(),
                _presentThread && GetCurrentThreadId() != _presentThread ? " (not the Present thread)" : "");
    if (_onBeforeResize) _onBeforeResize();  // ResizeBuffers fails while anyone holds a backbuffer reference
    HRESULT hr = _origResizeBuffers(sc, count, width, height, format, flags);
    if (FAILED(hr)) MC_LOG_WARN("render: ResizeBuffers returned 0x%08lx", static_cast<unsigned long>(hr));
    if (_onResize) _onResize(width, height);
    return hr;
}

// Creates a throwaway device + swapchain to read the DXGI swapchain vtable.
bool GetSwapChainVtable(void** out, size_t count) {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"JustAddMPDxgiProbe";
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"JustAddMP DXGI probe", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) {
        MC_LOG_ERROR("render: probe window creation failed (%lu)", GetLastError());
        return false;
    }

    DXGI_SWAP_CHAIN_DESC desc{};
    desc.BufferCount = 1;
    desc.BufferDesc.Width = 64;
    desc.BufferDesc.Height = 64;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* sc = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const D3D_DRIVER_TYPE types[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
    HRESULT hr = E_FAIL;
    for (D3D_DRIVER_TYPE type : types) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, type, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &sc,
                                           &device, nullptr, &context);
        if (SUCCEEDED(hr)) break;
    }
    bool ok = SUCCEEDED(hr) && sc;
    if (ok) {
        void** vtbl = *reinterpret_cast<void***>(sc);
        for (size_t i = 0; i < count; ++i) out[i] = vtbl[i];
        // Present1 only exists if the object is an IDXGISwapChain1 (it is on Win8+); verify before using slot 22.
        IDXGISwapChain1* sc1 = nullptr;
        if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain1), reinterpret_cast<void**>(&sc1)))) {
            out[kPresent1Index] = nullptr;
        } else {
            out[kPresent1Index] = (*reinterpret_cast<void***>(sc1))[kPresent1Index];
            sc1->Release();
        }
    } else {
        MC_LOG_ERROR("render: D3D11CreateDeviceAndSwapChain failed: 0x%08lx", static_cast<unsigned long>(hr));
    }
    if (sc) sc->Release();
    if (context) context->Release();
    if (device) device->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

} // namespace

bool InstallPresentHook(FrameCallback onFrame, ResizeCallback onResize, BeforeResizeCallback onBeforeResize,
                        DeviceLostCallback onDeviceLost) {
    _onBeforeResize = onBeforeResize;
    _onDeviceLost = onDeviceLost;
    _onFrame = onFrame;
    _onResize = onResize;
    QueryPerformanceFrequency(&_freq);

    void* vtbl[kPresent1Index + 1] = {};
    if (!GetSwapChainVtable(vtbl, kPresent1Index + 1)) return false;
    MC_LOG_INFO("render: swapchain vtable Present=%p ResizeBuffers=%p Present1=%p", vtbl[kPresentIndex],
                vtbl[kResizeBuffersIndex], vtbl[kPresent1Index]);

    if (!core::hook::Install("IDXGISwapChain::Present", vtbl[kPresentIndex], &PresentDetour, &_origPresent)) {
        return false;
    }
    if (!core::hook::Install("IDXGISwapChain::ResizeBuffers", vtbl[kResizeBuffersIndex], &ResizeBuffersDetour,
                             &_origResizeBuffers)) {
        MC_LOG_WARN("render: continuing without ResizeBuffers hook");
    }
    if (vtbl[kPresent1Index] && !core::hook::Install("IDXGISwapChain1::Present1", vtbl[kPresent1Index],
                                                     &Present1Detour, &_origPresent1)) {
        MC_LOG_WARN("render: continuing without Present1 hook");
    }
    return true;
}

void RemovePresentHook() {
    if (_origPresent1) core::hook::Remove("IDXGISwapChain1::Present1");
    if (_origResizeBuffers) core::hook::Remove("IDXGISwapChain::ResizeBuffers");
    if (_origPresent) core::hook::Remove("IDXGISwapChain::Present");
    _swapChain = nullptr;
}

IDXGISwapChain* GameSwapChain() { return _swapChain; }

} // namespace mc::render
