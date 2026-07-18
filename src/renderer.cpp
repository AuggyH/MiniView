#include "renderer.h"
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stdexcept>
#include <algorithm>
#include <string>

namespace mv {

namespace {
    constexpr float OVERLAY_PAD = 12.0f;
    constexpr float OVERLAY_FONT_SIZE = 14.0f;
}

Renderer::Renderer() = default;

Renderer::~Renderer() {
    discard_device_resources();
}

bool Renderer::init(HWND hwnd) {
    m_hwnd = hwnd;
    return create_device_resources();
}

bool Renderer::create_device_resources() {
    if (!m_hwnd) return false;

    RECT rc; GetClientRect(m_hwnd, &rc);
    m_target_size = {
        static_cast<uint32_t>(rc.right - rc.left),
        static_cast<uint32_t>(rc.bottom - rc.top)
    };
    if (m_target_size.width == 0 || m_target_size.height == 0) {
        m_target_size = {1200, 800};
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL feature_levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0
    };

    HRESULT hr = D3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        feature_levels, ARRAYSIZE(feature_levels),
        D3D11_SDK_VERSION, &m_d3d_device, nullptr, &m_d3d_context);
    if (FAILED(hr)) {
        // Try WARP (software) as fallback
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            feature_levels, ARRAYSIZE(feature_levels),
            D3D11_SDK_VERSION, &m_d3d_device, nullptr, &m_d3d_context);
        if (FAILED(hr)) return false;
    }

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
        m_d3d_device.Get(), m_hwnd, &scd, nullptr, nullptr, &m_swap_chain);
    if (FAILED(hr)) return false;

    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&m_d2d_factory));
    if (FAILED(hr)) return false;

    hr = m_d2d_factory->CreateDevice(dxgi_device.Get(), &m_d2d_device);
    if (FAILED(hr)) return false;

    hr = m_d2d_device->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2d_context);
    if (FAILED(hr)) return false;

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
    m_d2d_context->SetDpi(96.0f, 96.0f);
    m_d2d_context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

    return create_text_resources();
}

bool Renderer::create_text_resources() {
    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(m_dwrite_factory.GetAddressOf()));
    if (FAILED(hr)) return false;

    hr = m_dwrite_factory->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        OVERLAY_FONT_SIZE, L"en-US",
        &m_text_format);
    if (FAILED(hr)) return false;

    m_text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    if (m_d2d_context) {
        m_d2d_context->CreateSolidColorBrush(
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f),
            &m_overlay_brush);
    }
    return true;
}

void Renderer::discard_device_resources() {
    m_overlay_brush.Reset();
    m_text_format.Reset();
    m_dwrite_factory.Reset();
    m_image_bitmap.Reset();
    m_d2d_context.Reset();
    m_d2d_device.Reset();
    m_d2d_factory.Reset();
    m_swap_chain.Reset();
    m_d3d_context.Reset();
    m_d3d_device.Reset();
}

void Renderer::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
    m_target_size = {width, height};

    if (m_swap_chain && m_d2d_context) {
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

    uint32_t w, h;
    wic_bitmap->GetSize(&w, &h);
    m_img_width = w;
    m_img_height = h;

    m_image_bitmap.Reset();
    HRESULT hr = m_d2d_context->CreateBitmapFromWicBitmap(
        wic_bitmap, nullptr, &m_image_bitmap);
    if (FAILED(hr)) return;

    float sx = static_cast<float>(m_target_size.width)  / w;
    float sy = static_cast<float>(m_target_size.height) / h;
    m_scale = std::min(sx, sy);
    m_offset_x = 0;
    m_offset_y = 0;
    m_scroll_y = 0;
}

bool Renderer::begin_frame() {
    // Auto-recover from device loss
    if (!m_d2d_context) {
        if (!create_device_resources()) return false;
    }
    m_d2d_context->BeginDraw();
    return true;
}

bool Renderer::end_frame() {
    if (!m_d2d_context) return false;
    HRESULT hr = m_d2d_context->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        discard_device_resources();
        // Don't try to present — next frame will recreate
        return false;
    }
    if (FAILED(hr)) return false;

    DXGI_PRESENT_PARAMETERS pp = {};
    m_swap_chain->Present1(0, 0, &pp);
    return true;
}

void Renderer::clear(float r, float g, float b) {
    if (!m_d2d_context) return;
    m_d2d_context->Clear(D2D1::ColorF(r, g, b));
}

void Renderer::draw_image() {
    if (!m_d2d_context || !m_image_bitmap) return;

    float scaled_w = m_img_width  * m_scale;
    float scaled_h = m_img_height * m_scale;
    float x = (m_target_size.width  - scaled_w) / 2.0f + m_offset_x;
    float y = (m_target_size.height - scaled_h) / 2.0f + m_offset_y + m_scroll_y;

    D2D1_RECT_F dest = {x, y, x + scaled_w, y + scaled_h};
    D2D1_RECT_F src  = {0, 0,
        static_cast<float>(m_img_width),
        static_cast<float>(m_img_height)};

    m_d2d_context->DrawBitmap(
        m_image_bitmap.Get(), &dest, 1.0f,
        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC,
        &src);
}

void Renderer::draw_hint(const std::wstring& text) {
    if (!m_d2d_context || !m_text_format) return;
    ComPtr<ID2D1SolidColorBrush> brush;
    m_d2d_context->CreateSolidColorBrush(
        D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.8f), &brush);
    D2D1_RECT_F rc = {0, 0,
        static_cast<float>(m_target_size.width),
        static_cast<float>(m_target_size.height)};
    m_text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_d2d_context->DrawText(text.c_str(), static_cast<uint32_t>(text.size()),
        m_text_format.Get(), &rc, brush.Get());
    // Restore alignment for overlay
    m_text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

void Renderer::draw_overlay() {
    if (!m_d2d_context || !m_text_format || !m_overlay_brush) return;
    if (m_img_width == 0 || m_img_height == 0) return;

    int zoom_pct = static_cast<int>(m_scale * 100.0f + 0.5f);
    std::wstring text = std::to_wstring(zoom_pct) + L"%  |  " +
        std::to_wstring(m_img_width) + L"\u00D7" +
        std::to_wstring(m_img_height);

    float x = OVERLAY_PAD;
    float y = static_cast<float>(m_target_size.height) - OVERLAY_FONT_SIZE - OVERLAY_PAD - 4.0f;
    float max_w = static_cast<float>(m_target_size.width) - OVERLAY_PAD * 2;
    D2D1_RECT_F layout = {x, y, x + max_w, y + OVERLAY_FONT_SIZE + 4.0f};

    // Shadow
    ComPtr<ID2D1SolidColorBrush> shadow;
    m_d2d_context->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.6f), &shadow);
    D2D1_RECT_F sl = {x + 1.0f, y + 1.0f, x + max_w + 1.0f, y + OVERLAY_FONT_SIZE + 5.0f};
    m_d2d_context->DrawText(text.c_str(), static_cast<uint32_t>(text.size()),
        m_text_format.Get(), &sl, shadow.Get());
    m_d2d_context->DrawText(text.c_str(), static_cast<uint32_t>(text.size()),
        m_text_format.Get(), &layout, m_overlay_brush.Get());
}

void Renderer::set_scale(float s) {
    m_scale = std::max(0.01f, std::min(s, 100.0f));
}

void Renderer::set_offset(float x, float y) {
    m_offset_x = x;
    m_offset_y = y;
}

void Renderer::set_scroll_y(float y) {
    m_scroll_y = y;
}

// ── Grid drawing ─────────────────────────────────────────────

HRESULT Renderer::create_bitmap_from_wic(IWICBitmapSource* wic, ID2D1Bitmap1** out) {
    if (!m_d2d_context || !wic) return E_INVALIDARG;
    return m_d2d_context->CreateBitmapFromWicBitmap(wic, nullptr, out);
}

void Renderer::draw_grid_placeholder(float x, float y, float size,
                                      const std::wstring& name, bool selected) {
    if (!m_d2d_context) return;

    // Background
    D2D1_RECT_F rc = {x, y, x + size, y + size};
    float r = selected ? 0.20f : 0.15f;
    float g = selected ? 0.22f : 0.15f;
    float b = selected ? 0.25f : 0.15f;
    float radius = 4.0f;

    ComPtr<ID2D1SolidColorBrush> brush;
    m_d2d_context->CreateSolidColorBrush(D2D1::ColorF(r, g, b), &brush);
    m_d2d_context->FillRoundedRectangle(
        D2D1::RoundedRect(rc, radius, radius), brush.Get());

    // Selection border
    if (selected) {
        ComPtr<ID2D1SolidColorBrush> sel;
        m_d2d_context->CreateSolidColorBrush(
            D2D1::ColorF(0.35f, 0.55f, 0.85f), &sel);
        m_d2d_context->DrawRoundedRectangle(
            D2D1::RoundedRect(rc, radius, radius), sel.Get(), 2.0f);
    }

    // Filename text (centered at bottom)
    if (m_text_format && m_overlay_brush && !name.empty()) {
        float text_y = y + size - 20.0f;
        D2D1_RECT_F tr = {x + 3.0f, text_y, x + size - 3.0f, y + size - 2.0f};

        // Shadow
        ComPtr<ID2D1SolidColorBrush> shadow;
        m_d2d_context->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f), &shadow);
        D2D1_RECT_F sr = {x + 4.0f, text_y + 1.0f, x + size - 2.0f, y + size - 1.0f};
        m_d2d_context->DrawText(name.c_str(), static_cast<uint32_t>(name.size()),
            m_text_format.Get(), &sr, shadow.Get());
        m_d2d_context->DrawText(name.c_str(), static_cast<uint32_t>(name.size()),
            m_text_format.Get(), &tr, m_overlay_brush.Get());
    }
}

void Renderer::draw_grid_thumbnail(float x, float y, float size, ID2D1Bitmap1* thumb, bool square) {
    if (!m_d2d_context || !thumb) return;

    D2D1_SIZE_F bmp_size = thumb->GetSize();
    if (bmp_size.width == 0 || bmp_size.height == 0) return;

    if (square) {
        // Center-crop fill: scale to cover, clip to cell bounds
        float scale = std::max(size / bmp_size.width, size / bmp_size.height);
        float dw = bmp_size.width * scale;
        float dh = bmp_size.height * scale;
        float ox = x + (size - dw) / 2.0f;
        float oy = y + (size - dh) / 2.0f;

        D2D1_RECT_F clip = {x, y, x + size, y + size};
        m_d2d_context->PushAxisAlignedClip(&clip, D2D1_ANTIALIAS_MODE_ALIASED);
        D2D1_RECT_F dest = {ox, oy, ox + dw, oy + dh};
        m_d2d_context->DrawBitmap(thumb, &dest, 1.0f,
            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
        m_d2d_context->PopAxisAlignedClip();
    } else {
        // Scale to fit inside the cell, centered
        float scale = std::min(size / bmp_size.width, size / bmp_size.height);
        float dw = bmp_size.width * scale;
        float dh = bmp_size.height * scale;
        float ox = x + (size - dw) / 2.0f;
        float oy = y + (size - dh) / 2.0f;

        D2D1_RECT_F dest = {ox, oy, ox + dw, oy + dh};
        m_d2d_context->DrawBitmap(thumb, &dest, 1.0f,
            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
    }
}

} // namespace mv
