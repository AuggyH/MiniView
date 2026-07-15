#include "renderer.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stdexcept>

namespace mv {

namespace {
    constexpr float BG_R = 0.102f, BG_G = 0.102f, BG_B = 0.102f;  // #1a1a1a
}

Renderer::Renderer() = default;

Renderer::~Renderer() {
    discard_device_resources();
}

bool Renderer::create_device_resources(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    m_target_size = {
        static_cast<uint32_t>(rc.right - rc.left),
        static_cast<uint32_t>(rc.bottom - rc.top)
    };

    // ── D3D11 device ──
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
    };

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        feature_levels, ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION, &m_d3d_device, nullptr, &m_d3d_context);
    if (FAILED(hr)) return false;

    // ── DXGI swap chain ──
    ComPtr<IDXGIDevice> dxgi_device;
    m_d3d_device.As(&dxgi_device);

    ComPtr<IDXGIAdapter> dxgi_adapter;
    dxgi_device->GetAdapter(&dxgi_adapter);

    ComPtr<IDXGIFactory2> dxgi_factory;
    dxgi_adapter->GetParent(IID_PPV_ARGS(&dxgi_factory));

    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width       = m_target_size.width;
    scd.Height      = m_target_size.height;
    scd.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = 2;
    scd.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

    hr = dxgi_factory->CreateSwapChainForHwnd(
        m_d3d_device.Get(), hwnd, &scd, nullptr, nullptr, &m_swap_chain);
    if (FAILED(hr)) return false;

    // ── Direct2D factory + device ──
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&m_d2d_factory));
    if (FAILED(hr)) return false;

    hr = m_d2d_factory->CreateDevice(dxgi_device.Get(), &m_d2d_device);
    if (FAILED(hr)) return false;

    // ── D2D device context ──
    hr = m_d2d_device->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2d_context);
    if (FAILED(hr)) return false;

    // ── Set up render target from swap chain back buffer ──
    ComPtr<IDXGISurface> back_buffer;
    hr = m_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(hr)) return false;

    D2D1_BITMAP_PROPERTIES1 bp = {};
    bp.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

    ComPtr<ID2D1Bitmap1> target_bitmap;
    hr = m_d2d_context->CreateBitmapFromDxgiSurface(
        back_buffer.Get(), &bp, &target_bitmap);
    if (FAILED(hr)) return false;

    m_d2d_context->SetTarget(target_bitmap.Get());

    // ── High-quality rendering defaults ──
    m_d2d_context->SetDpi(96.0f, 96.0f);
    m_d2d_context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

    return true;
}

void Renderer::discard_device_resources() {
    m_image_bitmap.Reset();
    m_d2d_context.Reset();
    m_d2d_device.Reset();
    m_d2d_factory.Reset();
    m_swap_chain.Reset();
    m_d3d_context.Reset();
    m_d3d_device.Reset();

    m_img_width = m_img_height = 0;
}

bool Renderer::init(HWND hwnd) {
    return create_device_resources(hwnd);
}

void Renderer::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    if (width == m_target_size.width && height == m_target_size.height) return;

    m_target_size = {width, height};

    if (m_swap_chain) {
        m_d2d_context->SetTarget(nullptr);
        HRESULT hr = m_swap_chain->ResizeBuffers(
            2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
        if (SUCCEEDED(hr)) {
            ComPtr<IDXGISurface> back_buffer;
            hr = m_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
            if (SUCCEEDED(hr)) {
                D2D1_BITMAP_PROPERTIES1 bp = {};
                bp.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
                bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
                bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

                ComPtr<ID2D1Bitmap1> target;
                m_d2d_context->CreateBitmapFromDxgiSurface(
                    back_buffer.Get(), &bp, &target);
                m_d2d_context->SetTarget(target.Get());
            }
        }
    }
}

void Renderer::upload_image(IWICBitmapSource* wic_bitmap) {
    if (!m_d2d_context) return;

    // Get image dimensions
    uint32_t w, h;
    wic_bitmap->GetSize(&w, &h);
    m_img_width = w;
    m_img_height = h;

    // Upload FULL resolution to GPU texture
    m_image_bitmap.Reset();
    HRESULT hr = m_d2d_context->CreateBitmapFromWicBitmap(
        wic_bitmap, nullptr, &m_image_bitmap);
    if (FAILED(hr)) return;

    // Default: fit to window
    float scale_x = static_cast<float>(m_target_size.width)  / w;
    float scale_y = static_cast<float>(m_target_size.height) / h;
    m_scale = std::min(scale_x, scale_y);
    m_offset_x = 0;
    m_offset_y = 0;
}

bool Renderer::begin_frame() {
    if (!m_d2d_context) return false;
    m_d2d_context->BeginDraw();
    return true;
}

bool Renderer::end_frame() {
    if (!m_d2d_context) return false;
    HRESULT hr = m_d2d_context->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        discard_device_resources();
        return false;
    }
    // Present
    DXGI_PRESENT_PARAMETERS pp = {};
    m_swap_chain->Present1(0, 0, &pp);
    return SUCCEEDED(hr);
}

void Renderer::clear(float r, float g, float b) {
    if (!m_d2d_context) return;
    m_d2d_context->Clear(D2D1::ColorF(r, g, b));
}

void Renderer::draw_image() {
    if (!m_d2d_context || !m_image_bitmap) return;

    // Calculate centered position with scale
    float scaled_w = m_img_width  * m_scale;
    float scaled_h = m_img_height * m_scale;
    float x = (m_target_size.width  - scaled_w) / 2.0f + m_offset_x;
    float y = (m_target_size.height - scaled_h) / 2.0f + m_offset_y;

    D2D1_RECT_F dest = {x, y, x + scaled_w, y + scaled_h};
    D2D1_RECT_F src  = {0, 0,
        static_cast<float>(m_img_width),
        static_cast<float>(m_img_height)};

    // GPU high-quality cubic interpolation
    m_d2d_context->DrawBitmap(
        m_image_bitmap.Get(), &dest, 1.0f,
        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
        &src);
}

void Renderer::set_scale(float s) {
    m_scale = std::max(0.01f, std::min(s, 100.0f));
}

void Renderer::set_offset(float x, float y) {
    m_offset_x = x;
    m_offset_y = y;
}

} // namespace mv
