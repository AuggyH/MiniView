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
    m_d2d_context->SetDpi(m_dpi_x, m_dpi_y);
    m_d2d_context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

    return create_text_resources();
}

bool Renderer::create_text_resources() {
    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(m_dwrite_factory.GetAddressOf()));
    if (FAILED(hr)) return false;

    hr = m_dwrite_factory->CreateTextFormat(
        L"Microsoft YaHei", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        OVERLAY_FONT_SIZE * m_dpi_y / 96.0f, L"en-US",
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

    // Recalculate fit scale if we have an image loaded
    if (m_img_width > 0 && m_img_height > 0) {
        float sx = static_cast<float>(width)  / m_img_width;
        float sy = static_cast<float>(height) / m_img_height;
        m_fit_scale = std::min(sx, sy);
    }

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
    m_fit_scale = m_scale;
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

void Renderer::draw_info_card(const std::vector<std::pair<std::wstring, std::wstring>>& items) {
    if (!m_d2d_context || !m_text_format || items.empty()) return;

    float card_w = std::min(560.0f, static_cast<float>(m_target_size.width) - 40.0f);
    float line_h = 22.0f;
    float pad = 16.0f;
    float card_h = pad * 2 + line_h * items.size() + 8.0f;
    float card_x = (m_target_size.width - card_w) / 2.0f;
    float card_y = (m_target_size.height - card_h) / 2.0f;

    // Card background
    ComPtr<ID2D1SolidColorBrush> bg;
    m_d2d_context->CreateSolidColorBrush(
        D2D1::ColorF(0.08f, 0.08f, 0.10f, 0.92f), &bg);
    D2D1_RECT_F rc = {card_x, card_y, card_x + card_w, card_y + card_h};
    m_d2d_context->FillRoundedRectangle(
        D2D1::RoundedRect(rc, 8.0f, 8.0f), bg.Get());

    // Border
    ComPtr<ID2D1SolidColorBrush> border;
    m_d2d_context->CreateSolidColorBrush(
        D2D1::ColorF(0.25f, 0.25f, 0.30f, 0.8f), &border);
    m_d2d_context->DrawRoundedRectangle(
        D2D1::RoundedRect(rc, 8.0f, 8.0f), border.Get(), 1.0f);

    // Items
    ComPtr<ID2D1SolidColorBrush> label_brush, value_brush;
    m_d2d_context->CreateSolidColorBrush(
        D2D1::ColorF(0.5f, 0.5f, 0.55f, 1.0f), &label_brush);
    m_d2d_context->CreateSolidColorBrush(
        D2D1::ColorF(0.9f, 0.9f, 0.9f, 1.0f), &value_brush);

    float label_w = 80.0f;
    float value_x = card_x + pad + label_w + 8.0f;
    float value_w = card_w - pad * 2 - label_w - 8.0f;

    for (size_t i = 0; i < items.size(); ++i) {
        float y = card_y + pad + i * line_h;

        // Label
        D2D1_RECT_F lr = {card_x + pad, y, card_x + pad + label_w, y + line_h};
        m_d2d_context->DrawText(items[i].first.c_str(),
            static_cast<uint32_t>(items[i].first.size()),
            m_text_format.Get(), &lr, label_brush.Get());

        // Value
        if (!items[i].second.empty()) {
            D2D1_RECT_F vr = {value_x, y, value_x + value_w, y + line_h};
            m_d2d_context->DrawText(items[i].second.c_str(),
                static_cast<uint32_t>(items[i].second.size()),
                m_text_format.Get(), &vr, value_brush.Get());
        }
    }
}

void Renderer::draw_overlay() {
    if (!m_d2d_context || !m_text_format || !m_overlay_brush) return;
    if (m_img_width == 0 || m_img_height == 0) return;

    int zoom_pct = static_cast<int>(m_scale * 100.0f + 0.5f);
    std::wstring text = std::to_wstring(zoom_pct) + L"%  |  " +
        std::to_wstring(m_img_width) + L" \u00D7 " +
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
    m_scale = std::max(m_fit_scale, std::min(s, 100.0f));
}

void Renderer::set_offset(float x, float y) {
    m_offset_x = x;
    m_offset_y = y;
}

void Renderer::set_scroll_y(float y) {
    m_scroll_y = y;
}

void Renderer::set_dpi(float dpi_x, float dpi_y) {
    if (dpi_x > 0) m_dpi_x = dpi_x;
    if (dpi_y > 0) m_dpi_y = dpi_y;
    if (m_d2d_context) m_d2d_context->SetDpi(m_dpi_x, m_dpi_y);
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

void Renderer::draw_grid_thumbnail(float x, float y, float w, float h, ID2D1Bitmap1* thumb, bool square) {
    if (!m_d2d_context || !thumb || !m_d2d_factory) return;

    D2D1_SIZE_F bmp_size = thumb->GetSize();
    if (bmp_size.width == 0 || bmp_size.height == 0) return;

    float scale = square ? std::max(w / bmp_size.width, h / bmp_size.height)
                         : std::min(w / bmp_size.width, h / bmp_size.height);
    float dw = bmp_size.width * scale;
    float dh = bmp_size.height * scale;
    float ox = x + (w - dw) / 2.0f;
    float oy = y + (h - dh) / 2.0f;
    D2D1_RECT_F dest = {ox, oy, ox + dw, oy + dh};

    // Rounded corner clip
    float radius = 4.0f * m_dpi_y / 96.0f;
    D2D1_ROUNDED_RECT rr = {{x, y, x + w, y + h}, radius, radius};
    ComPtr<ID2D1RoundedRectangleGeometry> geo;
    m_d2d_factory->CreateRoundedRectangleGeometry(&rr, &geo);

    if (square) {
        D2D1_RECT_F clip = {x, y, x + w, y + h};
        m_d2d_context->PushAxisAlignedClip(&clip, D2D1_ANTIALIAS_MODE_ALIASED);
    }

    m_d2d_context->PushLayer(
        D2D1::LayerParameters(D2D1::InfiniteRect(), geo.Get(),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
            D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE),
        nullptr);
    m_d2d_context->DrawBitmap(thumb, &dest, 1.0f,
        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
    m_d2d_context->PopLayer();

    if (square) {
        m_d2d_context->PopAxisAlignedClip();
    }
}

void Renderer::draw_selection_border(D2D1_RECT_F rc) {
    if (!m_d2d_context) return;
    ComPtr<ID2D1SolidColorBrush> br;
    m_d2d_context->CreateSolidColorBrush(D2D1::ColorF(0.29f, 0.56f, 1.0f, 1.0f), &br);
    float r = 4.0f * m_dpi_y / 96.0f;
    float sw = 2.0f * m_dpi_y / 96.0f;
    float outer_r = r + sw;
    D2D1_ROUNDED_RECT rr = {rc, outer_r, outer_r};
    m_d2d_context->DrawRoundedRectangle(&rr, br.Get(), sw);
}

void Renderer::draw_label(float x, float y, float w, const std::wstring& text, float font_size,
    float cr, float cg, float cb) {
    if (!m_dwrite_factory || !m_d2d_context || text.empty()) return;
    ComPtr<IDWriteTextFormat> tf;
    float fs = font_size * m_dpi_y / 96.0f;
    m_dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        fs, L"en-US", &tf);
    tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    ComPtr<IDWriteTextLayout> layout;
    float max_h = fs * 3.0f;
    m_dwrite_factory->CreateTextLayout(text.c_str(), static_cast<uint32_t>(text.size()),
        tf.Get(), w, max_h, &layout);
    DWRITE_TRIMMING trim = {};
    trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    ComPtr<IDWriteInlineObject> ellipsis;
    m_dwrite_factory->CreateEllipsisTrimmingSign(tf.Get(), &ellipsis);
    layout->SetTrimming(&trim, ellipsis.Get());
    ComPtr<ID2D1SolidColorBrush> br;
    m_d2d_context->CreateSolidColorBrush(D2D1::ColorF(cr, cg, cb, 1.0f), &br);
    D2D1_POINT_2F pt = {x, y};
    m_d2d_context->DrawTextLayout(pt, layout.Get(), br.Get());
}

float Renderer::label_height(const std::wstring& text, float w, float font_size) {
    if (!m_dwrite_factory || text.empty()) return 0;
    ComPtr<IDWriteTextFormat> tf;
    float fs = font_size * m_dpi_y / 96.0f;
    m_dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        fs, L"en-US", &tf);
    ComPtr<IDWriteTextLayout> layout;
    float max_h = fs * 3.0f;  // 2 lines max
    m_dwrite_factory->CreateTextLayout(text.c_str(), static_cast<uint32_t>(text.size()),
        tf.Get(), w, max_h, &layout);
    DWRITE_TRIMMING trim = {};
    trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    ComPtr<IDWriteInlineObject> ellipsis;
    m_dwrite_factory->CreateEllipsisTrimmingSign(tf.Get(), &ellipsis);
    layout->SetTrimming(&trim, ellipsis.Get());
    DWRITE_TEXT_METRICS m;
    layout->GetMetrics(&m);
    return m.height;
}

void Renderer::draw_side_panel(float x, float y_off, float w, float h,
    ID2D1Bitmap1* preview, uint32_t pw, uint32_t ph,
    const std::vector<std::pair<std::wstring, std::wstring>>& info,
    const std::vector<std::pair<std::wstring, std::wstring>>& gen_info,
    std::vector<std::pair<D2D1_RECT_F, std::wstring>>* out_clickable,
    int sel_idx, const std::wstring* toast)
{
    if (!m_d2d_context || !m_text_format) return;

    float dpi_s = m_dpi_y / 96.0f;
    float pad   = 24.0f * dpi_s;
    float gap   = 8.0f * dpi_s;
    float sec_gap = 24.0f * dpi_s;

    float y0 = y_off;
    float y = y0 + pad;

    // Panel background
    ComPtr<ID2D1SolidColorBrush> bg;
    m_d2d_context->CreateSolidColorBrush(D2D1::ColorF(0.10f, 0.10f, 0.12f, 0.95f), &bg);
    D2D1_RECT_F rc = {x, y_off, x + w, y_off + h};
    m_d2d_context->FillRectangle(&rc, bg.Get());

    // Divider line
    ComPtr<ID2D1SolidColorBrush> line;
    m_d2d_context->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.20f, 0.24f, 1.0f), &line);
    m_d2d_context->DrawLine({x, y_off}, {x, y_off + h}, line.Get(), 1.0f);

    ComPtr<ID2D1SolidColorBrush> label_br, value_br;
    m_d2d_context->CreateSolidColorBrush(D2D1::ColorF(0.80f, 0.80f, 0.82f, 1.0f), &label_br);
    m_d2d_context->CreateSolidColorBrush(D2D1::ColorF(0.60f, 0.60f, 0.64f, 1.0f), &value_br);

    float content_w = w - pad * 2;

    // Preview thumbnail — fill content width, height from native aspect ratio
    if (preview && pw > 0 && ph > 0) {
        float thumb_w = content_w;
        float dw = thumb_w;
        float dh = thumb_w * ph / pw;
        float ox = x + pad;
        float oy = y;
        D2D1_RECT_F dest = {ox, oy, ox + dw, oy + dh};
        {
            float pr = 4.0f * dpi_s;
            D2D1_ROUNDED_RECT prr = {{ox, oy, ox + dw, oy + dh}, pr, pr};
            ComPtr<ID2D1RoundedRectangleGeometry> pgeo;
            m_d2d_factory->CreateRoundedRectangleGeometry(&prr, &pgeo);
            m_d2d_context->PushLayer(
                D2D1::LayerParameters(D2D1::InfiniteRect(), pgeo.Get(),
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                    D2D1::IdentityMatrix(), 1.0f, nullptr, D2D1_LAYER_OPTIONS_NONE),
                nullptr);
            m_d2d_context->DrawBitmap(preview, &dest, 1.0f,
                D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
            m_d2d_context->PopLayer();
        }
        y = y_off + pad + dh + sec_gap;
    }

    // ── Info rows (label emphasized, value slightly dimmed) ──
    float lw = 70.0f * dpi_s;
    float cgap = 8.0f * dpi_s;
    float val_w = content_w - lw - cgap;

    for (auto& [label, value] : info) {
        if (y + gap > y_off + h) break;
        int cur_idx = out_clickable ? static_cast<int>(out_clickable->size()) : -1;
        float y1 = draw_text_line(x + pad, y, lw,      label, label_br.Get(), 10.0f);
        float y2 = draw_text_line(x + pad + lw + cgap, y, val_w,
                                  value, value_br.Get(), 10.0f);
        if (out_clickable && !value.empty()) {
            D2D1_RECT_F cr = {x + pad + lw + cgap, y, x + pad + lw + cgap + val_w, y2};
            out_clickable->push_back({cr, value});
            if (sel_idx == cur_idx) {
                float tw = measure_text(value, 10.0f * dpi_s);
                float hw = (tw + 10.0f * dpi_s > val_w) ? val_w : tw + 10.0f * dpi_s;
                D2D1_RECT_F hr = {x + pad + lw + cgap, y, x + pad + lw + cgap + hw, y2};
                D2D1_ROUNDED_RECT hrr = {hr, 2.0f * dpi_s, 2.0f * dpi_s};
                ComPtr<ID2D1SolidColorBrush> sel_br;
                m_d2d_context->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.40f, 0.70f, 0.35f), &sel_br);
                m_d2d_context->FillRoundedRectangle(&hrr, sel_br.Get());
                draw_text_line(x + pad + lw + cgap, y, val_w, value, value_br.Get(), 10.0f);
            }
        }
        y = std::max(y1, y2) + gap - 4.0f * dpi_s;
    }

    // ── Generation info section ──
    if (!gen_info.empty()) {
        float title_pad = 12.0f * dpi_s;
        y += title_pad;
        // Horizontal divider
        ComPtr<ID2D1SolidColorBrush> div_br;
        m_d2d_context->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.20f, 0.24f, 1.0f), &div_br);
        m_d2d_context->DrawLine({x + pad, y}, {x + pad + content_w, y}, div_br.Get(), 1.0f);
        y += title_pad;
        // Section title
        {
            ComPtr<ID2D1SolidColorBrush> title_br;
            m_d2d_context->CreateSolidColorBrush(D2D1::ColorF(0.70f, 0.70f, 0.74f, 1.0f), &title_br);
            float ty = draw_text_line(x + pad, y, content_w, L"\u751F\u6210\u4FE1\u606F",
                                      title_br.Get(), 12.0f);
            y = ty + title_pad;
        }
        for (auto& [label, value] : gen_info) {
            if (y + gap > y_off + h) break;
            int cur_idx = out_clickable ? static_cast<int>(out_clickable->size()) : -1;
            float y1 = draw_text_line(x + pad, y, lw,      label, label_br.Get(), 10.0f);
            float y2 = draw_text_line(x + pad + lw + cgap, y, val_w,
                                      value, value_br.Get(), 10.0f);
            if (out_clickable && !value.empty()) {
                D2D1_RECT_F cr = {x + pad + lw + cgap, y, x + pad + lw + cgap + val_w, y2};
                out_clickable->push_back({cr, value});
                if (sel_idx == cur_idx) {
                    float tw = measure_text(value, 10.0f * dpi_s);
                    float hw = (tw + 10.0f * dpi_s > val_w) ? val_w : tw + 10.0f * dpi_s;
                    D2D1_RECT_F hr = {x + pad + lw + cgap, y, x + pad + lw + cgap + hw, y2};
                    D2D1_ROUNDED_RECT hrr = {hr, 2.0f * dpi_s, 2.0f * dpi_s};
                    ComPtr<ID2D1SolidColorBrush> sel_br;
                    m_d2d_context->CreateSolidColorBrush(D2D1::ColorF(0.20f, 0.40f, 0.70f, 0.35f), &sel_br);
                    m_d2d_context->FillRoundedRectangle(&hrr, sel_br.Get());
                    draw_text_line(x + pad + lw + cgap, y, val_w, value, value_br.Get(), 10.0f);
                }
            }
            y = std::max(y1, y2) + gap - 4.0f * dpi_s;
        }
    }

    // Toast notification
    if (toast && !toast->empty()) {
        float toast_h = 28.0f * dpi_s;
        float toast_y = y_off + h - pad - toast_h;
        D2D1_RECT_F tr = {x + pad, toast_y, x + w - pad, toast_y + toast_h};
        ComPtr<ID2D1SolidColorBrush> toast_bg;
        m_d2d_context->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.15f, 0.18f, 0.92f), &toast_bg);
        D2D1_ROUNDED_RECT trr = {tr, 4.0f * dpi_s, 4.0f * dpi_s};
        m_d2d_context->FillRoundedRectangle(&trr, toast_bg.Get());
        ComPtr<ID2D1SolidColorBrush> toast_txt;
        m_d2d_context->CreateSolidColorBrush(D2D1::ColorF(0.85f, 0.85f, 0.88f, 1.0f), &toast_txt);
        draw_text_line(x + pad + 10.0f * dpi_s, toast_y + (toast_h - 13.0f * dpi_s) / 2.0f,
                       w - pad * 2 - 20.0f * dpi_s, *toast, toast_txt.Get(), 11.0f);
    }
}

void Renderer::draw_scrollbar(float x, float y, float w, float h,
    float total, float view, float pos, bool active)
{
    if (!m_d2d_context || total <= 0 || view >= total) return;

    float ratio = view / total;
    float thumb_h = std::max(28.0f, h * ratio);
    float range = total - view;
    float pct = (range > 0) ? std::min(1.0f, pos / range) : 0.0f;
    float thumb_y = y + (h - thumb_h) * pct;

    float inner_w  = active ? w - 2.0f : w - 6.0f;  // wider when active
    float offset_x = (w - inner_w) / 2.0f;
    float alpha    = active ? 0.95f : 0.70f;
    float r = active ? 0.58f : 0.40f;
    float g = active ? 0.58f : 0.40f;
    float b = active ? 0.64f : 0.45f;

    ComPtr<ID2D1SolidColorBrush> thumb;
    m_d2d_context->CreateSolidColorBrush(D2D1::ColorF(r, g, b, alpha), &thumb);

    D2D1_RECT_F tr = {x + offset_x, thumb_y, x + offset_x + inner_w, thumb_y + thumb_h};
    float radius = inner_w * 0.4f;
    D2D1_ROUNDED_RECT rr = {tr, radius, radius};
    m_d2d_context->FillRoundedRectangle(&rr, thumb.Get());
}

float Renderer::draw_text_line(float x, float y, float w,
    const std::wstring& text, ID2D1SolidColorBrush* brush,
    float font_size)
{
    if (!m_dwrite_factory || !m_d2d_context || text.empty()) return y + 20;

    // Use m_text_format with default font size, or create a sized one
    IDWriteTextFormat* fmt = m_text_format.Get();
    ComPtr<IDWriteTextFormat> sized_fmt;
    if (font_size > 0.0f && m_dwrite_factory) {
        m_dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            font_size * m_dpi_y / 96.0f, L"en-US", &sized_fmt);
        if (sized_fmt) fmt = sized_fmt.Get();
    }

    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = m_dwrite_factory->CreateTextLayout(
        text.c_str(), static_cast<uint32_t>(text.size()),
        fmt, w, 200.0f, &layout);
    if (FAILED(hr)) return y + 20;

    DWRITE_TEXT_METRICS metrics;
    layout->GetMetrics(&metrics);
    float h = metrics.height;

    D2D1_POINT_2F origin = {x, y};
    m_d2d_context->DrawTextLayout(origin, layout.Get(), brush);
    return y + h + 4;  // 4px gap
}

float Renderer::measure_text(const std::wstring& text, float font_size) {
    if (!m_dwrite_factory) return 0;
    ComPtr<IDWriteTextFormat> tf;
    m_dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        font_size, L"en-US", &tf);
    ComPtr<IDWriteTextLayout> layout;
    m_dwrite_factory->CreateTextLayout(text.c_str(), static_cast<uint32_t>(text.size()),
        tf.Get(), 2000.0f, 30.0f, &layout);
    DWRITE_TEXT_METRICS m;
    layout->GetMetrics(&m);
    return m.width;
}

void Renderer::draw_toolbar(float w, const std::vector<std::wstring>& items, int active_idx) {
    if (!m_d2d_context || !m_dwrite_factory) return;

    float h = 28.0f * m_dpi_y / 96.0f;
    ComPtr<ID2D1SolidColorBrush> bg, text_brush, hover_bg;
    m_d2d_context->CreateSolidColorBrush(D2D1::ColorF(0.11f, 0.11f, 0.13f, 1.0f), &bg);
    m_d2d_context->CreateSolidColorBrush(D2D1::ColorF(0.8f, 0.8f, 0.82f, 1.0f), &text_brush);
    m_d2d_context->CreateSolidColorBrush(D2D1::ColorF(0.18f, 0.18f, 0.22f, 1.0f), &hover_bg);

    D2D1_RECT_F rc = {0, 0, w, h};
    m_d2d_context->FillRectangle(&rc, bg.Get());

    ComPtr<IDWriteTextFormat> tf;
    m_dwrite_factory->CreateTextFormat(L"Microsoft YaHei", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        13.0f * m_dpi_y / 96.0f, L"en-US", &tf);

    float x = 12.0f;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        ComPtr<IDWriteTextLayout> layout;
        m_dwrite_factory->CreateTextLayout(items[i].c_str(),
            static_cast<uint32_t>(items[i].size()), tf.Get(), 200.0f, 30.0f, &layout);
        DWRITE_TEXT_METRICS m;
        layout->GetMetrics(&m);
        float iw = m.width + 24.0f;

        if (i == active_idx) {
            D2D1_RECT_F hr = {x, 2, x + iw, h - 2};
            m_d2d_context->FillRectangle(&hr, hover_bg.Get());
        }

        D2D1_POINT_2F pt = {x + 12.0f, (h - m.height) / 2.0f};
        m_d2d_context->DrawTextLayout(pt, layout.Get(), text_brush.Get());
        x += iw;
    }
}

} // namespace mv
