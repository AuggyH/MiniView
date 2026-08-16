#include "renderer.h"
#include "renderer_state.h"
#include <dwrite_1.h>
#include <fstream>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <stdexcept>
#include <algorithm>
#include <iterator>
#include <limits>
#include <string>
#include <array>
#include <span>

namespace mv {

namespace {
    constexpr float OVERLAY_PAD = dt::kSpaceMdDip;
    constexpr float OVERLAY_FONT_SIZE = dt::kFontSizeXlDip;

    // Bounded caches (linear search). The brush and text-format caches are
    // small and hot; the rounded-geometry cache is size-keyed and is also
    // cleared on resize/device loss. A full cache is cleared wholesale —
    // callers hold ComPtr copies while drawing, so a mid-frame clear is safe.
    constexpr size_t kSolidBrushCacheMax = 256;
    constexpr size_t kTextFormatCacheMax = 64;
    constexpr size_t kRoundedGeometryCacheMax = 128;

    bool same_color(D2D1_COLOR_F left, D2D1_COLOR_F right) {
        return left.r == right.r && left.g == right.g
            && left.b == right.b && left.a == right.a;
    }

    D2D1_RECT_F to_d2d_rect(ComicRenderRect rect) {
        return {rect.left, rect.top, rect.right, rect.bottom};
    }
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
    discard_device_resources();
    auto fail = [this]() {
        discard_device_resources();
        return false;
    };

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
        m_d3d_context.Reset();
        m_d3d_device.Reset();
        hr = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags,
            feature_levels, ARRAYSIZE(feature_levels),
            D3D11_SDK_VERSION, &m_d3d_device, nullptr, &m_d3d_context);
        if (FAILED(hr)) return fail();
    }

    ComPtr<IDXGIDevice> dxgi_device;
    hr = m_d3d_device.As(&dxgi_device);
    if (FAILED(hr)) return fail();

    ComPtr<IDXGIAdapter> dxgi_adapter;
    hr = dxgi_device->GetAdapter(&dxgi_adapter);
    if (FAILED(hr)) return fail();

    ComPtr<IDXGIFactory2> dxgi_factory;
    hr = dxgi_adapter->GetParent(IID_PPV_ARGS(&dxgi_factory));
    if (FAILED(hr)) return fail();

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
    if (FAILED(hr)) return fail();

    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, IID_PPV_ARGS(&m_d2d_factory));
    if (FAILED(hr)) return fail();

    hr = m_d2d_factory->CreateDevice(dxgi_device.Get(), &m_d2d_device);
    if (FAILED(hr)) return fail();

    hr = m_d2d_device->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &m_d2d_context);
    if (FAILED(hr)) return fail();

    ComPtr<IDXGISurface> back_buffer;
    hr = m_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (FAILED(hr)) return fail();

    D2D1_BITMAP_PROPERTIES1 bp = {};
    bp.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

    ComPtr<ID2D1Bitmap1> target_bitmap;
    hr = m_d2d_context->CreateBitmapFromDxgiSurface(
        back_buffer.Get(), &bp, &target_bitmap);
    if (FAILED(hr)) return fail();

    m_d2d_context->SetTarget(target_bitmap.Get());
    m_d2d_context->SetDpi(m_dpi_x, m_dpi_y);
    m_d2d_context->SetUnitMode(D2D1_UNIT_MODE_PIXELS);

    if (!create_text_resources()) return fail();
    ++m_device_generation;
    return true;
}

bool Renderer::create_text_resources() {
    ComPtr<IDWriteFactory> dwrite_factory;
    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwrite_factory.GetAddressOf()));
    if (FAILED(hr)) return false;

    ComPtr<IDWriteTextFormat> text_format;
    hr = dwrite_factory->CreateTextFormat(
        dt::kFontFamilyUi, nullptr,
        dt::kFontWeightNormal,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        dt::dip(OVERLAY_FONT_SIZE, m_dpi_y), L"en-US",
        &text_format);
    if (FAILED(hr)) return false;

    hr = text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    if (FAILED(hr)) return false;
    hr = text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    if (FAILED(hr)) return false;

    if (!m_d2d_context) return false;
    ComPtr<ID2D1SolidColorBrush> overlay_brush;
    hr = m_d2d_context->CreateSolidColorBrush(
        dt::d2d(dt::kColorOverlayText),
        &overlay_brush);
    if (FAILED(hr)) return false;

    m_dwrite_factory = dwrite_factory;
    m_text_format = text_format;
    m_overlay_brush = overlay_brush;
    return true;
}

ComPtr<ID2D1SolidColorBrush> Renderer::get_solid_brush(
    const D2D1_COLOR_F& color) {
    if (!m_d2d_context) return nullptr;
    for (const auto& entry : m_solid_brushes) {
        if (same_color(entry.color, color)) return entry.brush;
    }
    if (m_solid_brushes.size() >= kSolidBrushCacheMax) {
        m_solid_brushes.clear();
    }
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(m_d2d_context->CreateSolidColorBrush(color, &brush))) {
        return nullptr;
    }
    m_solid_brushes.push_back({color, brush});
    return brush;
}

ComPtr<IDWriteTextFormat> Renderer::get_text_format(
    const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
    DWRITE_FONT_STYLE style, DWRITE_FONT_STRETCH stretch,
    const wchar_t* locale) {
    if (!m_dwrite_factory || !family || !locale) return nullptr;
    for (const auto& entry : m_text_formats) {
        if (entry.family == family && entry.size == size
            && entry.weight == weight && entry.style == style
            && entry.stretch == stretch && entry.locale == locale) {
            // The mutable layout fields below are the only ones call sites
            // change after creation; reset them so the shared object behaves
            // exactly like a freshly created format.
            entry.format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            entry.format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            entry.format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            return entry.format;
        }
    }
    if (m_text_formats.size() >= kTextFormatCacheMax) {
        m_text_formats.clear();
    }
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(m_dwrite_factory->CreateTextFormat(
            family, nullptr, weight, style, stretch, size, locale,
            &format)) || !format) {
        return nullptr;
    }
    m_text_formats.push_back(
        {family, size, weight, style, stretch, locale, format});
    return format;
}

ComPtr<ID2D1RoundedRectangleGeometry> Renderer::get_rounded_geometry(
    float width, float height, float radius) {
    if (!m_d2d_factory) return nullptr;
    for (const auto& entry : m_rounded_geometries) {
        if (entry.width == width && entry.height == height
            && entry.radius == radius) {
            return entry.geometry;
        }
    }
    if (m_rounded_geometries.size() >= kRoundedGeometryCacheMax) {
        m_rounded_geometries.clear();
    }
    // Geometries are cached at the origin and placed with LayerParameters'
    // maskTransform, so entries are keyed by size only and stay valid for
    // any item position.
    const D2D1_ROUNDED_RECT rounded = {
        {0.0f, 0.0f, width, height}, radius, radius};
    ComPtr<ID2D1RoundedRectangleGeometry> geometry;
    if (FAILED(m_d2d_factory->CreateRoundedRectangleGeometry(
            &rounded, &geometry)) || !geometry) {
        return nullptr;
    }
    m_rounded_geometries.push_back({width, height, radius, geometry});
    return geometry;
}

void Renderer::discard_device_resources() {
    m_solid_brushes.clear();
    m_text_formats.clear();
    m_rounded_geometries.clear();
    m_overlay_brush.Reset();
    m_text_format.Reset();
    m_dwrite_factory.Reset();
    m_image_bitmap.Reset();
    m_image_scaled.Reset();
    m_image_scaled_scale = -1.0f;
    // Device-scoped resources beyond the image itself: after a device
    // recreation these would otherwise keep pointing at the old device
    // and fail (or draw garbage) on the next frame.
    m_placeholder_bitmap.Reset();
    m_filmstrip_bg_gradient.Reset();
    m_filmstrip_mask_gradient.Reset();
    m_filmstrip_mask_dpi = 0.0f;
    m_filmstrip_mask_width = -1.0f;
    m_filmstrip_mask_layer.Reset();
    m_d2d_context.Reset();
    m_d2d_device.Reset();
    m_d2d_factory.Reset();
    m_swap_chain.Reset();
    m_d3d_context.Reset();
    m_d3d_device.Reset();
}

bool Renderer::resize(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    m_target_size = {width, height};

    // Size-keyed rounded geometries are cached at the origin and reused via
    // LayerParameters maskTransform. Clear on every resize so entries can
    // never be stale after a target-size change (safer than keying by the
    // full target size; entries are cheap to recreate).
    m_rounded_geometries.clear();

    update_fit_scale();

    if (!m_swap_chain || !m_d2d_context) return false;

    m_d2d_context->SetTarget(nullptr);
    HRESULT hr = m_swap_chain->ResizeBuffers(
        2, width, height, DXGI_FORMAT_B8G8R8A8_UNORM, 0);
    if (should_recreate_render_device(hr)) {
        discard_device_resources();
        return false;
    }

    ComPtr<IDXGISurface> back_buffer;
    hr = m_swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (should_recreate_render_device(hr)) {
        discard_device_resources();
        return false;
    }

    D2D1_BITMAP_PROPERTIES1 bp = {};
    bp.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    bp.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    ComPtr<ID2D1Bitmap1> target;
    hr = m_d2d_context->CreateBitmapFromDxgiSurface(
        back_buffer.Get(), &bp, &target);
    if (should_recreate_render_device(hr) || !target) {
        discard_device_resources();
        return false;
    }

    m_d2d_context->SetTarget(target.Get());
    m_d2d_context->SetDpi(m_dpi_x, m_dpi_y);
    return true;
}

bool Renderer::upload_image(IWICBitmapSource* wic_bitmap, bool reset_view) {
    if (!m_d2d_context || !wic_bitmap) return false;

    uint32_t w, h;
    HRESULT hr = wic_bitmap->GetSize(&w, &h);
    if (FAILED(hr) || w == 0 || h == 0) return false;

    ComPtr<ID2D1Bitmap1> image_bitmap;
    hr = m_d2d_context->CreateBitmapFromWicBitmap(
        wic_bitmap, nullptr, &image_bitmap);
    if (FAILED(hr) || !image_bitmap) return false;

    m_image_bitmap = image_bitmap;
    // The scaled-image cache is stale: force a rebuild on next draw.
    m_image_scaled.Reset();
    m_image_scaled_scale = -1.0f;
    m_img_width = w;
    m_img_height = h;

    update_fit_scale();
    if (reset_view) {
        m_scale = m_fit_scale;
        m_offset_x = 0;
        m_offset_y = 0;
        m_scroll_y = 0;
    }
    return true;
}

bool Renderer::begin_frame() {
    // Auto-recover from device loss
    if (!m_d2d_context) {
        if (!create_device_resources()) return false;
    }
    if (!m_d2d_context) return false;
    m_d2d_context->BeginDraw();
    return true;
}

bool Renderer::end_frame() {
    if (!m_d2d_context) return false;
    HRESULT hr = m_d2d_context->EndDraw();
    if (should_recreate_render_device(hr)) {
        discard_device_resources();
        return false;
    }

    DXGI_PRESENT_PARAMETERS pp = {};
    hr = m_swap_chain->Present1(0, 0, &pp);
    if (should_recreate_render_device(hr)) {
        discard_device_resources();
        return false;
    }
    return true;
}

void Renderer::clear(float r, float g, float b) {
    if (!m_d2d_context) return;
    m_d2d_context->Clear(D2D1::ColorF(r, g, b, 1.0f));
}


void Renderer::draw_image() {
    if (!m_d2d_context) return;
    if (m_placeholder_bitmap) {
        // Provisional upscaled thumbnail while the full decode is in flight
        // (progressive paging: instant feedback, replaced on upload).
        const D2D1_SIZE_F bsz = m_placeholder_bitmap->GetSize();
        if (bsz.width <= 0.0f || bsz.height <= 0.0f) return;
        const float avail_w = content_width();
        const float avail_h =
            static_cast<float>(m_target_size.height) - m_content_top;
        const float fit = std::min(avail_w / bsz.width, avail_h / bsz.height);
        const float w = bsz.width * fit;
        const float h = bsz.height * fit;
        const float x = m_content_left + (avail_w - w) / 2.0f;
        const float y = m_content_top + (avail_h - h) / 2.0f;
        const D2D1_RECT_F dest = {x, y, x + w, y + h};
        m_d2d_context->DrawBitmap(m_placeholder_bitmap.Get(), &dest, 1.0f,
            D2D1_INTERPOLATION_MODE_LINEAR, nullptr);
        return;
    }
    if (!m_image_bitmap) return;

    float scaled_w = m_img_width  * m_scale;
    float scaled_h = m_img_height * m_scale;
    float x = m_content_left + (content_width() - scaled_w) / 2.0f + m_offset_x;
    float y = m_content_top + (m_target_size.height - m_content_top - scaled_h) / 2.0f + m_offset_y + m_scroll_y;

    D2D1_RECT_F dest = {x, y, x + scaled_w, y + scaled_h};


    // Scaled-image cache: the 4K source bitmap is re-sampled ONLY when the
    // zoom scale changes (in ensure_image_scaled, outside the draw
    // session); panning/scrolling and every animation frame then blit the
    // pre-scaled bitmap 1:1 — a cheap GPU copy instead of a full
    // 4096x4096 LINEAR resize per frame.
    if (m_image_scaled && m_image_scaled_scale == m_scale) {
        m_d2d_context->DrawBitmap(m_image_scaled.Get(), &dest, 1.0f,
            D2D1_INTERPOLATION_MODE_LINEAR, nullptr);
    } else {
        D2D1_RECT_F src = {0, 0,
            static_cast<float>(m_img_width),
            static_cast<float>(m_img_height)};
        m_d2d_context->DrawBitmap(
            m_image_bitmap.Get(), &dest, 1.0f,
            D2D1_INTERPOLATION_MODE_LINEAR,
            &src);
    }
}

void Renderer::set_placeholder(ID2D1Bitmap1* bmp) {
    m_placeholder_bitmap = bmp;
}

void Renderer::clear_placeholder() {
    m_placeholder_bitmap.Reset();
}

void Renderer::ensure_image_scaled() {
    if (!m_d2d_context || !m_image_bitmap) return;
    if (m_image_scaled && m_image_scaled_scale == m_scale) return;
    const float w = m_img_width * m_scale;
    const float h = m_img_height * m_scale;
    if (w <= 0.0f || h <= 0.0f || w > 16384.0f || h > 16384.0f) return;
    ComPtr<ID2D1Bitmap1> target_bitmap;
    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
            D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(m_d2d_context->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(std::ceil(w)),
                static_cast<UINT32>(std::ceil(h))),
            nullptr, 0, &props, &target_bitmap)))
        return;
    ComPtr<ID2D1Image> old_target;
    m_d2d_context->GetTarget(&old_target);
    m_d2d_context->SetTarget(target_bitmap.Get());
    m_d2d_context->BeginDraw();
    m_d2d_context->DrawBitmap(m_image_bitmap.Get(),
        D2D1::RectF(0, 0, w, h), 1.0f, D2D1_INTERPOLATION_MODE_LINEAR,
        D2D1::RectF(0, 0, static_cast<float>(m_img_width),
            static_cast<float>(m_img_height)));
    HRESULT hr = m_d2d_context->EndDraw();
    m_d2d_context->SetTarget(old_target.Get());
    if (SUCCEEDED(hr)) {
        m_image_scaled = target_bitmap;
        m_image_scaled_scale = m_scale;
    }
}

void Renderer::draw_hint(const std::wstring& text) {
    if (!m_d2d_context || !m_text_format) return;
    auto brush = get_solid_brush(dt::d2d(dt::kColorHintText));
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

void Renderer::draw_status_message(const std::wstring& text) {
    if (!m_d2d_context || !m_text_format || text.empty()) return;

    const float dpi_scale = m_dpi_y / 96.0f;
    const float available_width = std::max(1.0f, content_width() - dt::kSpace2xlDip * dpi_scale);
    const float width = std::min(dt::kStatusMessageMaxWidthDip * dpi_scale, available_width);
    const float height = dt::kSize44Dip * dpi_scale;
    const float left = (content_width() - width) * 0.5f;
    const float top = m_content_top + dt::kSpaceMdDip * dpi_scale;
    const D2D1_RECT_F bounds = {left, top, left + width, top + height};
    const auto rounded = D2D1::RoundedRect(
        bounds, dt::kStatusMessageCornerRadiusDip * dpi_scale,
        dt::kStatusMessageCornerRadiusDip * dpi_scale);

    auto background = get_solid_brush(dt::d2d(dt::kColorStatusErrorBg));
    auto border = get_solid_brush(dt::d2d(dt::kColorStatusErrorBorder));
    auto foreground = get_solid_brush(dt::d2d(dt::kColorStatusErrorText));
    if (!background || !border || !foreground) return;

    m_d2d_context->FillRoundedRectangle(rounded, background.Get());
    m_d2d_context->DrawRoundedRectangle(
        rounded, border.Get(), std::max(1.0f, dpi_scale));
    m_text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    m_d2d_context->DrawText(text.c_str(), static_cast<uint32_t>(text.size()),
        m_text_format.Get(), &bounds, foreground.Get());
    m_text_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    m_text_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

void Renderer::draw_info_card(const std::vector<std::pair<std::wstring, std::wstring>>& items) {
    if (!m_d2d_context || !m_dwrite_factory || items.empty()) return;

    float dpi_s = m_dpi_y / 96.0f;
    float font_size = dt::kFontSizeLgDip;           // logical pt, draw_text_line will DPI-scale
    float pad = dt::kSpaceLgDip * dpi_s;
    float label_w = layout::kPanelLabelColumnWidthDip * dpi_s;
    float gap = dt::kSpace10Dip * dpi_s;         // label–value gap
    float item_spacing = dt::kSpaceSmDip * dpi_s;
    float card_w = std::min(dt::kInfoCardMaxWidthDip * dpi_s, m_target_size.width - dt::kSpace40Dip * dpi_s);
    float value_w = card_w - pad * 2 - label_w - gap;
    float min_h = font_size * dpi_s * 1.6f;
    int   max_lines = 3;

    // ── Pass 1: measure item heights ──
    std::vector<float> heights;
    heights.reserve(items.size());
    for (auto& [label, value] : items) {
        float h = label_height(value, value_w, font_size, max_lines);
        if (h < min_h) h = min_h;
        heights.push_back(h);
    }

    float card_h = pad * 2;
    for (float h : heights) card_h += h + item_spacing;
    if (!heights.empty()) card_h -= item_spacing;  // no trailing gap

    // Clamp to viewport
    float max_card_h = m_target_size.height - dt::kSpace40Dip * dpi_s;
    if (card_h > max_card_h) card_h = max_card_h;

    float card_x = (m_target_size.width - card_w) * 0.5f;
    float card_y = (m_target_size.height - card_h) * 0.5f;
    float radius = dt::kInfoCardCornerRadiusDip * dpi_s;

    // ── Card background ──
    auto bg = get_solid_brush(dt::d2d(dt::kColorInfoCardBg));
    auto border = get_solid_brush(dt::d2d(dt::kColorInfoCardBorder));

    D2D1_RECT_F rc = {card_x, card_y, card_x + card_w, card_y + card_h};
    D2D1_ROUNDED_RECT rr = {rc, radius, radius};
    m_d2d_context->FillRoundedRectangle(&rr, bg.Get());
    m_d2d_context->DrawRoundedRectangle(&rr, border.Get(), 1.0f * dpi_s);

    // ── Brushes ──
    auto label_brush = get_solid_brush(dt::d2d(dt::kColorInfoLabel));
    auto value_brush = get_solid_brush(dt::d2d(dt::kColorInfoValue));
    auto dim_brush = get_solid_brush(dt::d2d(dt::kColorInfoDim));

    // ── Pass 2: draw items ──
    float cur_y = card_y + pad;
    for (size_t i = 0; i < items.size(); ++i) {
        float h = heights[i];

        // Label (top-aligned, single-line)
        D2D1_RECT_F lr = {card_x + pad, cur_y, card_x + pad + label_w, cur_y + min_h};
        m_d2d_context->DrawText(items[i].first.c_str(),
            static_cast<uint32_t>(items[i].first.size()),
            m_text_format.Get(), &lr, label_brush.Get());

        // Value (wrapped, up to max_lines)
        if (!items[i].second.empty()) {
            int lines = (h > min_h * 1.1f) ? max_lines : 1;
            draw_text_line(card_x + pad + label_w + gap, cur_y, value_w,
                items[i].second, value_brush.Get(), font_size, nullptr, lines);
        }

        cur_y += h + item_spacing;
    }

    // ── Hint: press Esc or click to dismiss ──
    float hint_size = dt::kFontSizeXsDip * dpi_s;
    auto hint_fmt = get_text_format(dt::kFontFamilyUi, hint_size,
        dt::kFontWeightNormal, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, L"en-US");
    if (hint_fmt) hint_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    std::wstring hint = L"\u6309 I \u5173\u95ED";  // 按 I 关闭
    D2D1_RECT_F hr = {card_x, card_y + card_h + dt::kSpaceXsDip * dpi_s,
                      card_x + card_w, card_y + card_h + dt::kSpace20Dip * dpi_s};
    m_d2d_context->DrawText(hint.c_str(), static_cast<uint32_t>(hint.size()),
        hint_fmt.Get(), &hr, dim_brush.Get());
}

void Renderer::draw_overlay(float bottom_inset) {
    if (!m_d2d_context || !m_text_format || !m_overlay_brush) return;
    if (m_img_width == 0 || m_img_height == 0) return;

    int zoom_pct = static_cast<int>(m_scale * 100.0f + 0.5f);
    std::wstring text = std::to_wstring(zoom_pct) + L"%  |  " +
        std::to_wstring(m_img_width) + L" \u00D7 " +
        std::to_wstring(m_img_height);

    float x = OVERLAY_PAD;
    // Lift the overlay above the filmstrip when it occupies the bottom edge.
    float inset = std::max(0.0f, bottom_inset);
    float y = static_cast<float>(m_target_size.height) - inset
        - OVERLAY_FONT_SIZE - OVERLAY_PAD - 4.0f;
    float max_w = static_cast<float>(m_target_size.width) - OVERLAY_PAD * 2;
    D2D1_RECT_F layout = {x, y, x + max_w, y + OVERLAY_FONT_SIZE + 4.0f};

    // Shadow
    auto shadow = get_solid_brush(dt::d2d(dt::kColorOverlayShadow));
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

void Renderer::set_content_viewport(float top, float right, float left) {
    m_content_top = std::max(0.0f, top);
    m_content_right = std::max(0.0f, right);
    m_content_left = std::max(0.0f, left);
    update_fit_scale();
}

float Renderer::content_width() const {
    return std::max(1.0f,
        static_cast<float>(m_target_size.width) - m_content_right
            - m_content_left);
}

void Renderer::update_fit_scale() {
    if (m_img_width == 0 || m_img_height == 0) return;
    float sx = content_width() / static_cast<float>(m_img_width);
    float content_height = std::max(1.0f,
        static_cast<float>(m_target_size.height) - m_content_top);
    float sy = content_height / static_cast<float>(m_img_height);
    m_fit_scale = std::min(sx, sy);
    if (m_scale < m_fit_scale) m_scale = m_fit_scale;
}

void Renderer::set_scroll_y(float y) {
    m_scroll_y = y;
}

void Renderer::set_dpi(float dpi_x, float dpi_y) {
    if (dpi_x > 0) m_dpi_x = dpi_x;
    if (dpi_y > 0) m_dpi_y = dpi_y;
    if (m_d2d_context) {
        m_d2d_context->SetDpi(m_dpi_x, m_dpi_y);
        create_text_resources();
    }
}

// ── Grid drawing ─────────────────────────────────────────────

HRESULT Renderer::create_bitmap_from_wic(IWICBitmapSource* wic, ID2D1Bitmap1** out) {
    if (!m_d2d_context || !wic) return E_INVALIDARG;
    return m_d2d_context->CreateBitmapFromWicBitmap(wic, nullptr, out);
}

void Renderer::draw_grid_placeholder(float x, float y, float w, float h, D2D1_COLOR_F color) {
    if (!m_d2d_context) return;

    // Keep an unloaded or very dark thumbnail visibly distinct from the
    // #1A1A1A canvas so a fast scroll never looks like an empty grid.
    if (std::max({color.r, color.g, color.b}) < 0.18f)
        color = dt::d2d(dt::kColorPlaceholderMin);

    float radius = layout::kThumbCornerRadiusDip * m_dpi_y / 96.0f;
    D2D1_RECT_F rc = {x, y, x + w, y + h};
    D2D1_ROUNDED_RECT rr = {rc, radius, radius};

    auto brush = get_solid_brush(color);
    m_d2d_context->FillRoundedRectangle(&rr, brush.Get());
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

    // Rounded corner clip (geometry cached at the origin, positioned via
    // the layer mask transform).
    float radius = layout::kThumbCornerRadiusDip * m_dpi_y / 96.0f;
    auto geo = get_rounded_geometry(w, h, radius);

    if (square) {
        D2D1_RECT_F clip = {x, y, x + w, y + h};
        m_d2d_context->PushAxisAlignedClip(&clip, D2D1_ANTIALIAS_MODE_ALIASED);
    }

    m_d2d_context->PushLayer(
        D2D1::LayerParameters(D2D1::InfiniteRect(), geo.Get(),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
            D2D1::Matrix3x2F::Translation(x, y), 1.0f, nullptr,
            D2D1_LAYER_OPTIONS_NONE),
        nullptr);
    m_d2d_context->DrawBitmap(thumb, &dest, 1.0f,
        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
    m_d2d_context->PopLayer();

    if (square) {
        m_d2d_context->PopAxisAlignedClip();
    }
}

void Renderer::draw_favourite_badge(float x, float y, float size) {
    if (!m_d2d_context || !m_dwrite_factory || size <= 0.0f) return;
    auto chip = get_solid_brush(dt::d2d(dt::kColorFavouriteChip));
    auto heart = get_solid_brush(dt::d2d(dt::kColorFavouriteHeart));
    const D2D1_RECT_F rect = D2D1::RectF(x, y, x + size, y + size);
    const float radius = size * dt::kFavouriteBadgeRadiusFraction;
    m_d2d_context->FillRoundedRectangle(
        D2D1::RoundedRect(rect, radius, radius), chip.Get());
    m_d2d_context->DrawRoundedRectangle(
        D2D1::RoundedRect(rect, radius, radius), heart.Get(), 1.0f);
    const std::wstring glyph = L"\u2665";
    const float dpi_s = m_dpi_y > 0.0f ? m_dpi_y / 96.0f : 1.0f;
    const float fs = size * dt::kFavouriteBadgeGlyphSizeFraction / dpi_s;
    const float gw = measure_text(glyph, fs * dpi_s);
    const float gh = label_height(glyph, gw + 4.0f, fs, 1);
    draw_text_line(x + (size - gw) * 0.5f, y + (size - gh) * 0.5f,
        gw + 4.0f, glyph, heart.Get(), fs, nullptr, 1);
}

void Renderer::draw_comic_page(
    ID2D1Bitmap1* bitmap, D2D1_RECT_F destination) {
    draw_comic_bitmap(
        bitmap, destination,
        comic_render_metrics(m_dpi_y, false).corner_radius);
}

void Renderer::draw_comic_bitmap(
    ID2D1Bitmap1* bitmap, D2D1_RECT_F destination,
    float corner_radius) {
    if (!m_d2d_context || !bitmap) return;
    if (destination.right <= destination.left
        || destination.bottom <= destination.top) return;

    if (corner_radius <= 0.0f || !m_d2d_factory) {
        m_d2d_context->DrawBitmap(
            bitmap, &destination, 1.0f,
            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
        return;
    }

    const float width = destination.right - destination.left;
    const float height = destination.bottom - destination.top;
    auto geometry = get_rounded_geometry(width, height, corner_radius);
    if (!geometry) {
        m_d2d_context->DrawBitmap(
            bitmap, &destination, 1.0f,
            D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
        return;
    }

    m_d2d_context->PushLayer(
        D2D1::LayerParameters(
            D2D1::InfiniteRect(), geometry.Get(),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
            D2D1::Matrix3x2F::Translation(
                destination.left, destination.top),
            1.0f, nullptr,
            D2D1_LAYER_OPTIONS_NONE),
        nullptr);
    m_d2d_context->DrawBitmap(
        bitmap, &destination, 1.0f,
        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
    m_d2d_context->PopLayer();
}

void Renderer::draw_comic_card(D2D1_RECT_F destination, bool failed) {
    draw_comic_card_visual(
        destination, destination,
        failed ? ComicPageVisual::Error : ComicPageVisual::Placeholder,
        comic_render_metrics(m_dpi_y, false));
}

void Renderer::draw_comic_card_visual(
    D2D1_RECT_F destination, D2D1_RECT_F text_bounds,
    ComicPageVisual visual, const ComicRenderMetrics& metrics) {
    if (!m_d2d_context || destination.right <= destination.left
        || destination.bottom <= destination.top) return;

    const bool failed = visual == ComicPageVisual::Error;
    auto background = get_solid_brush(
        failed ? dt::d2d(dt::kColorComicErrorBg)
               : dt::d2d(dt::kColorComicPlaceholderBg));
    if (!background) return;

    if (metrics.corner_radius > 0.0f) {
        const D2D1_ROUNDED_RECT rounded = {
            destination, metrics.corner_radius, metrics.corner_radius};
        m_d2d_context->FillRoundedRectangle(&rounded, background.Get());
    } else {
        m_d2d_context->FillRectangle(&destination, background.Get());
    }

    auto border = get_solid_brush(
        failed ? dt::d2d(dt::kColorComicErrorBorder)
               : dt::d2d(dt::kColorComicPlaceholderBorder));
    if (border) {
        if (metrics.corner_radius > 0.0f) {
            const D2D1_ROUNDED_RECT rounded = {
                destination, metrics.corner_radius, metrics.corner_radius};
            m_d2d_context->DrawRoundedRectangle(
                &rounded, border.Get(), metrics.card_border_width);
        } else {
            m_d2d_context->DrawRectangle(
                &destination, border.Get(), metrics.card_border_width);
        }
    }
    if (!failed || !m_dwrite_factory) return;

    auto text = get_solid_brush(dt::d2d(dt::kColorComicErrorText));
    auto format = get_text_format(
        dt::kFontFamilyUi, metrics.error_font_size, dt::kFontWeightNormal,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, L"zh-CN");
    if (!format) return;
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    D2D1_RECT_F padded_text = {
        text_bounds.left + metrics.card_padding,
        text_bounds.top + metrics.card_padding,
        text_bounds.right - metrics.card_padding,
        text_bounds.bottom - metrics.card_padding};
    if (padded_text.right <= padded_text.left
        || padded_text.bottom <= padded_text.top) {
        padded_text = text_bounds;
    }
    constexpr wchar_t message[] = L"\u56FE\u7247\u52A0\u8F7D\u5931\u8D25";
    m_d2d_context->DrawText(
        message, static_cast<UINT32>(std::size(message) - 1),
        format.Get(), &padded_text, text.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Renderer::draw_comic_pages(
    std::span<const ComicPageDrawItem> pages,
    ComicRenderViewport viewport) {
    if (!m_d2d_context) return;
    if (!std::isfinite(viewport.dpi) || viewport.dpi <= 0.0f) {
        viewport.dpi = m_dpi_y;
    }

    std::vector<ComicPageRenderInput> inputs;
    inputs.reserve(pages.size());
    for (const ComicPageDrawItem& page : pages) {
        const ComicPageVisual visual = page.failed || page.geometry.decode_failed
            ? ComicPageVisual::Error
            : (page.bitmap ? ComicPageVisual::Bitmap
                           : ComicPageVisual::Placeholder);
        inputs.push_back({page.geometry, visual});
    }

    const ComicRenderPlan plan = build_comic_render_plan(inputs, viewport);
    if (plan.viewport.empty()) return;

    const D2D1_RECT_F viewport_rect = to_d2d_rect(plan.viewport);
    auto background = get_solid_brush(dt::d2d(dt::kColorCanvas));
    if (background) {
        m_d2d_context->FillRectangle(&viewport_rect, background.Get());
    }

    m_d2d_context->PushAxisAlignedClip(
        &viewport_rect, D2D1_ANTIALIAS_MODE_ALIASED);
    for (const ComicPageRenderCommand& command : plan.pages) {
        const ComicPageDrawItem& page = pages[command.input_index];
        const D2D1_RECT_F destination = to_d2d_rect(command.destination);
        const D2D1_RECT_F clip = to_d2d_rect(command.clip);
        m_d2d_context->PushAxisAlignedClip(
            &clip, D2D1_ANTIALIAS_MODE_ALIASED);
        if (command.visual == ComicPageVisual::Bitmap && page.bitmap) {
            draw_comic_bitmap(
                page.bitmap, destination, plan.metrics.corner_radius);
        } else {
            draw_comic_card_visual(
                destination, clip, command.visual, plan.metrics);
        }
        m_d2d_context->PopAxisAlignedClip();
    }
    m_d2d_context->PopAxisAlignedClip();
}

void Renderer::draw_comic_controls(const ComicControlsRenderInput& input) {
    if (!m_d2d_context || !m_dwrite_factory) return;
    const ComicControlsLayout layout = build_comic_controls_layout(input);
    if (layout.viewport.empty()) return;

    const D2D1_RECT_F viewport = to_d2d_rect(layout.viewport);
    m_d2d_context->PushAxisAlignedClip(
        &viewport, D2D1_ANTIALIAS_MODE_ALIASED);

    if (layout.scrollbar.visible) {
        auto track_brush = get_solid_brush(
            dt::d2d(dt::kColorComicScrollbarTrack));
        const D2D1_COLOR_F thumb_color = layout.scrollbar.dragging
            ? dt::d2d(dt::kColorComicScrollbarThumbActive)
            : layout.scrollbar.hovered
                ? dt::d2d(dt::kColorComicScrollbarThumbHover)
                : dt::d2d(dt::kColorComicScrollbarThumbIdle);
        auto thumb_brush = get_solid_brush(thumb_color);
        if (track_brush) {
            const D2D1_ROUNDED_RECT track = {
                to_d2d_rect(layout.scrollbar.track),
                layout.metrics.scrollbar_radius,
                layout.metrics.scrollbar_radius};
            m_d2d_context->FillRoundedRectangle(&track, track_brush.Get());
        }
        if (thumb_brush) {
            const D2D1_ROUNDED_RECT thumb = {
                to_d2d_rect(layout.scrollbar.thumb),
                layout.metrics.scrollbar_radius,
                layout.metrics.scrollbar_radius};
            m_d2d_context->FillRoundedRectangle(&thumb, thumb_brush.Get());
        }
    }

    const auto draw_text_overlay = [this, &layout](
        const ComicTextOverlayLayout& overlay, float font_size,
        float padding, float background_alpha) {
        if (!overlay.visible || overlay.bounds.empty() || overlay.text.empty()
            || overlay.text.size()
                > static_cast<std::size_t>(std::numeric_limits<UINT32>::max())) {
            return;
        }

        const D2D1_RECT_F bounds = to_d2d_rect(overlay.bounds);
        auto background = get_solid_brush(
            dt::d2d(dt::with_alpha(dt::kColorComicOverlayBg, background_alpha)));
        auto border = get_solid_brush(dt::d2d(dt::kColorComicOverlayBorder));
        auto text = get_solid_brush(dt::d2d(dt::kColorComicOverlayText));
        const float radius = std::min(
            overlay.bounds.height() * 0.28f,
            dt::kComicOverlayCornerRadiusMaxDip * layout.metrics.dpi_scale);
        const D2D1_ROUNDED_RECT rounded = {bounds, radius, radius};
        if (background) {
            m_d2d_context->FillRoundedRectangle(&rounded, background.Get());
        }
        if (border) {
            m_d2d_context->DrawRoundedRectangle(
                &rounded, border.Get(),
                std::max(1.0f, layout.metrics.dpi_scale));
        }
        if (!text) return;

        auto format = get_text_format(
            dt::kFontFamilyUi, font_size, dt::kFontWeightNormal,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, L"zh-CN");
        if (!format) return;
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

        const float text_width = std::max(
            1.0f, overlay.bounds.width() - 2.0f * padding);
        const float text_height = std::max(1.0f, overlay.bounds.height());
        ComPtr<IDWriteTextLayout> text_layout;
        m_dwrite_factory->CreateTextLayout(
            overlay.text.data(), static_cast<UINT32>(overlay.text.size()),
            format.Get(), text_width, text_height, &text_layout);
        if (!text_layout) return;
        DWRITE_TRIMMING trimming = {};
        trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        ComPtr<IDWriteInlineObject> ellipsis;
        m_dwrite_factory->CreateEllipsisTrimmingSign(
            format.Get(), &ellipsis);
        if (ellipsis) text_layout->SetTrimming(&trimming, ellipsis.Get());
        const D2D1_POINT_2F origin = {
            overlay.bounds.left + padding, overlay.bounds.top};
        m_d2d_context->DrawTextLayout(
            origin, text_layout.Get(), text.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    draw_text_overlay(
        layout.page_badge, layout.metrics.page_badge_font_size,
        layout.metrics.page_badge_padding, 0.72f);
    draw_text_overlay(
        layout.transient_overlay, layout.metrics.page_toast_font_size,
        layout.metrics.page_toast_padding, 0.88f);

    if (layout.autoscroll.visible) {
        const D2D1_POINT_2F anchor = {
            layout.autoscroll.anchor_x, layout.autoscroll.anchor_y};
        const D2D1_ELLIPSE dead_zone = {
            anchor, layout.autoscroll.dead_zone_radius,
            layout.autoscroll.dead_zone_radius};
        const D2D1_ELLIPSE anchor_dot = {
            anchor, layout.metrics.autoscroll_anchor_radius,
            layout.metrics.autoscroll_anchor_radius};
        auto zone = get_solid_brush(dt::d2d(dt::kColorAutoscrollZone));
        auto ring = get_solid_brush(dt::d2d(dt::kColorAutoscrollRing));
        const float arrow_alpha = 0.72f + 0.24f * layout.autoscroll.intensity;
        auto arrow = get_solid_brush(
            dt::d2d(dt::with_alpha(dt::kColorAutoscrollArrow, arrow_alpha)));
        if (zone) {
            m_d2d_context->FillEllipse(&dead_zone, zone.Get());
            m_d2d_context->FillEllipse(&anchor_dot, zone.Get());
        }
        if (ring) {
            m_d2d_context->DrawEllipse(
                &dead_zone, ring.Get(), layout.metrics.autoscroll_stroke_width);
            m_d2d_context->DrawEllipse(
                &anchor_dot, ring.Get(), layout.metrics.autoscroll_stroke_width);
            const float chevron = layout.metrics.autoscroll_arrow_head * 0.65f;
            const float center_gap = layout.metrics.dpi_scale;
            m_d2d_context->DrawLine(
                {anchor.x - chevron, anchor.y - center_gap},
                {anchor.x, anchor.y - chevron}, ring.Get(),
                layout.metrics.autoscroll_stroke_width);
            m_d2d_context->DrawLine(
                {anchor.x, anchor.y - chevron},
                {anchor.x + chevron, anchor.y - center_gap}, ring.Get(),
                layout.metrics.autoscroll_stroke_width);
            m_d2d_context->DrawLine(
                {anchor.x - chevron, anchor.y + center_gap},
                {anchor.x, anchor.y + chevron}, ring.Get(),
                layout.metrics.autoscroll_stroke_width);
            m_d2d_context->DrawLine(
                {anchor.x, anchor.y + chevron},
                {anchor.x + chevron, anchor.y + center_gap}, ring.Get(),
                layout.metrics.autoscroll_stroke_width);
        }
        if (arrow && layout.autoscroll.direction
                != ComicAutoscrollDirection::Stationary) {
            const float stroke = layout.metrics.autoscroll_stroke_width
                * (1.0f + 0.5f * layout.autoscroll.intensity);
            const D2D1_POINT_2F tail = {
                anchor.x, layout.autoscroll.arrow_tail_y};
            const D2D1_POINT_2F tip = {
                anchor.x, layout.autoscroll.arrow_tip_y};
            m_d2d_context->DrawLine(tail, tip, arrow.Get(), stroke);
            const float head_y = layout.autoscroll.direction
                    == ComicAutoscrollDirection::Backward
                ? tip.y + layout.autoscroll.arrow_head
                : tip.y - layout.autoscroll.arrow_head;
            m_d2d_context->DrawLine(
                tip, {tip.x - layout.autoscroll.arrow_head, head_y},
                arrow.Get(), stroke);
            m_d2d_context->DrawLine(
                tip, {tip.x + layout.autoscroll.arrow_head, head_y},
                arrow.Get(), stroke);
        }
    }

    // Page-width drag slider (explicit mouse-direct width control).
    if (layout.width_slider.visible) {
        const ComicWidthSliderLayout& slider = layout.width_slider;
        auto track = get_solid_brush(dt::d2d(dt::kColorWidthSliderTrack));
        auto thumb = get_solid_brush(dt::d2d(dt::kColorWidthSliderThumb));
        auto label = get_solid_brush(dt::d2d(dt::kColorTextDim));
        const float slider_radius = dt::kComicWidthSliderTrackRadiusPx;
        m_d2d_context->FillRoundedRectangle(
            D2D1::RoundedRect(
                D2D1::RectF(slider.track.left, slider.track.top,
                    slider.track.right, slider.track.bottom),
                slider_radius, slider_radius),
            track.Get());
        const D2D1_ELLIPSE knob = {
            {slider.thumb_x, slider.thumb_y},
            slider.thumb_radius, slider.thumb_radius};
        m_d2d_context->FillEllipse(&knob, thumb.Get());
        const std::wstring label_text = L"页宽";
        const float dpi_s = m_dpi_y / 96.0f;
        const float lw = measure_text(label_text, dt::kFontSizeSmDip * dpi_s);
        const float lh = label_height(label_text, lw + dt::kSpaceXsDip, dt::kFontSizeSmDip, 1);
        draw_text_line(slider.track.left - lw - dt::kSpaceSmDip * dpi_s,
            slider.thumb_y - lh * 0.5f, lw + dt::kSpaceXsDip, label_text,
            label.Get(), dt::kFontSizeSmDip, nullptr, 1);
    }

    // Persistent cruise indicator (visible while cruising or paused).
    if (input.cruise_active || input.cruise_paused) {
        const std::wstring text = input.cruise_paused
            ? L"⏸ 已暂停 (P 继续)"
            : L"▶ 巡航中";
        const float dpi_s = m_dpi_y / 96.0f;
        const float fs = dt::kFontSizeSmDip;
        const float tw2 = measure_text(text, fs * dpi_s);
        auto cruise_br = get_solid_brush(dt::d2d(dt::kColorCruiseIndicator));
        const float th2 = label_height(text, tw2 + dt::kSpaceXsDip, fs, 1);
        draw_text_line(
            layout.viewport.right - layout.metrics.edge_margin
                - tw2 - dt::kSpaceSmDip * dpi_s,
            layout.viewport.top + layout.metrics.edge_margin,
            tw2 + dt::kSpaceXsDip, text, cruise_br.Get(), fs, nullptr, 1);
        (void)th2;
    }

    m_d2d_context->PopAxisAlignedClip();
}

void Renderer::draw_selection_border(D2D1_RECT_F rc, float alpha) {
    if (!m_d2d_context) return;
    auto br = get_solid_brush(
        dt::d2d(dt::with_alpha(dt::kColorSelectionAccent, alpha)));
    float r = layout::kThumbCornerRadiusDip * m_dpi_y / 96.0f;
    float sw = dt::kSelectionBorderWidthDip * m_dpi_y / 96.0f;
    float outer_r = r + sw;
    D2D1_ROUNDED_RECT rr = {rc, outer_r, outer_r};
    m_d2d_context->DrawRoundedRectangle(&rr, br.Get(), sw);
}

void Renderer::draw_label(float x, float y, float w, const std::wstring& text, float font_size,
    float cr, float cg, float cb) {
    if (!m_dwrite_factory || !m_d2d_context || text.empty()) return;
    float fs = font_size * m_dpi_y / 96.0f;
    auto tf = get_text_format(dt::kFontFamilyUi, fs,
        dt::kFontWeightNormal, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, L"en-US");
    if (tf) tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    ComPtr<IDWriteTextLayout> layout;
    float max_h = fs * 3.0f;
    m_dwrite_factory->CreateTextLayout(text.c_str(), static_cast<uint32_t>(text.size()),
        tf.Get(), w, max_h, &layout);
    DWRITE_TRIMMING trim = {};
    trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    ComPtr<IDWriteInlineObject> ellipsis;
    m_dwrite_factory->CreateEllipsisTrimmingSign(tf.Get(), &ellipsis);
    layout->SetTrimming(&trim, ellipsis.Get());
    auto br = get_solid_brush(D2D1::ColorF(cr, cg, cb, 1.0f));
    D2D1_POINT_2F pt = {x, y};
    m_d2d_context->DrawTextLayout(pt, layout.Get(), br.Get());
}

float Renderer::label_height(const std::wstring& text, float w, float font_size, int max_lines) {
    if (!m_dwrite_factory || text.empty()) return 0;
    float fs = font_size * m_dpi_y / 96.0f;
    auto tf = get_text_format(dt::kFontFamilyUi, fs,
        dt::kFontWeightNormal, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, L"en-US");
    ComPtr<IDWriteTextLayout> layout;
    float max_h = (max_lines > 0) ? fs * 1.4f * max_lines : 1000.0f;
    m_dwrite_factory->CreateTextLayout(text.c_str(), static_cast<uint32_t>(text.size()),
        tf.Get(), w, max_h, &layout);
    if (max_lines > 0) {
        DWRITE_TRIMMING trim = {};
        trim.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        ComPtr<IDWriteInlineObject> ellipsis;
        m_dwrite_factory->CreateEllipsisTrimmingSign(tf.Get(), &ellipsis);
        ComPtr<IDWriteTextLayout1> layout1;
        layout.As(&layout1);
        if (layout1) layout1->SetTrimming(&trim, ellipsis.Get());
    }
    DWRITE_TEXT_METRICS m;
    layout->GetMetrics(&m);
    return m.height;
}

float Renderer::draw_side_panel(float x, float y_off, float w, float h,
    ID2D1Bitmap1* preview, uint32_t pw, uint32_t ph,
    const std::vector<std::pair<std::wstring, std::wstring>>& info,
    const std::vector<std::pair<std::wstring, std::wstring>>& gen_info,
    std::vector<PanelRegion>* out_clickable,
    int sel_idx, const std::wstring* toast, float scroll_y)
{
    if (!m_d2d_context || !m_text_format) return 0.0f;

    float dpi_s = m_dpi_y / 96.0f;
    float pad   = dt::kSpaceXlDip * dpi_s;
    float gap   = dt::kSpaceSmDip * dpi_s;
    float sec_gap = dt::kSpaceXlDip * dpi_s;

    float y0 = y_off;
    float y = y0 + pad - scroll_y;

    // Clip drawing to panel area
    D2D1_RECT_F clip_rect = {x, y_off, x + w, y_off + h};
    m_d2d_context->PushAxisAlignedClip(&clip_rect, D2D1_ANTIALIAS_MODE_ALIASED);

    // Panel background (fixed, not scrolled)
    auto bg = get_solid_brush(dt::d2d(dt::kColorPanelBg));
    D2D1_RECT_F rc = {x, y_off, x + w, y_off + h};
    m_d2d_context->FillRectangle(&rc, bg.Get());

    // Divider line
    auto line = get_solid_brush(dt::d2d(dt::kColorPanelDivider));
    m_d2d_context->DrawLine({x, y_off}, {x, y_off + h}, line.Get(), 1.0f);

    auto label_br = get_solid_brush(dt::d2d(dt::kColorPanelLabel));
    auto value_br = get_solid_brush(dt::d2d(dt::kColorPanelValue));

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
            float pr = dt::kPanelPreviewCornerRadiusDip * dpi_s;
            auto pgeo = get_rounded_geometry(dw, dh, pr);
            m_d2d_context->PushLayer(
                D2D1::LayerParameters(D2D1::InfiniteRect(), pgeo.Get(),
                    D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                    D2D1::Matrix3x2F::Translation(ox, oy), 1.0f, nullptr,
                    D2D1_LAYER_OPTIONS_NONE),
                nullptr);
            m_d2d_context->DrawBitmap(preview, &dest, 1.0f,
                D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
            m_d2d_context->PopLayer();
        }
        y += dh + sec_gap;  // track from scrolled position
    }

    // ── Info rows (label emphasized, value slightly dimmed) ──
    float lw = layout::kPanelLabelColumnWidthDip * dpi_s;
    float cgap = dt::kSpaceSmDip * dpi_s;
    float val_w = content_w - lw - cgap;


    for (auto& [label, value] : info) {
        if (y + gap > y_off + h) break;
        int cur_idx = out_clickable ? static_cast<int>(out_clickable->size()) : -1;
        float y1 = draw_text_line(x + pad, y, lw,      label, label_br.Get(), dt::kFontSizeXsDip);
        float vw = 0;
        float y2 = draw_text_line(x + pad + lw + cgap, y, val_w,
                                  value, value_br.Get(), dt::kFontSizeXsDip, &vw, 10);
        if (out_clickable && !value.empty()) {
            D2D1_RECT_F cr = {x + pad + lw + cgap, y, x + pad + lw + cgap + val_w, y2};
            out_clickable->push_back({cr, value, label});
            if (sel_idx == cur_idx) {
                float hw = std::min(vw + dt::kSpaceSmDip * dpi_s, val_w);
                float hx = x + pad + lw + cgap - dt::kSpaceXsDip * dpi_s;
                D2D1_RECT_F hr = {hx, y, hx + hw + dt::kSpaceXsDip * dpi_s, y2};
                D2D1_ROUNDED_RECT hrr = {
                    hr, dt::kPanelSelectionCornerRadiusDip * dpi_s,
                    dt::kPanelSelectionCornerRadiusDip * dpi_s};
                auto sel_br = get_solid_brush(dt::d2d(dt::kColorPanelSelection));
                m_d2d_context->FillRoundedRectangle(&hrr, sel_br.Get());
                draw_text_line(x + pad + lw + cgap, y, val_w, value, value_br.Get(), dt::kFontSizeXsDip, nullptr, 10);
            }
        }
        y = std::max(y1, y2) + gap - dt::kSpaceXsDip * dpi_s;
    }

    // ── Generation info section ──
    if (!gen_info.empty()) {
        float title_pad = dt::kSpaceMdDip * dpi_s;
        y += title_pad;
        // Horizontal divider
        auto div_br = get_solid_brush(dt::d2d(dt::kColorPanelDivider));
        m_d2d_context->DrawLine({x + pad, y}, {x + pad + content_w, y}, div_br.Get(), 1.0f);
        y += title_pad;
        // Section title
        {
            auto title_br = get_solid_brush(dt::d2d(dt::kColorPanelSectionTitle));
            float ty = draw_text_line(x + pad, y, content_w, L"\u751F\u6210\u4FE1\u606F",
                                      title_br.Get(), dt::kFontSizeMdDip);
            y = ty + title_pad;
        }
        for (auto& [label, value] : gen_info) {
            if (label.empty()) {
                y = draw_text_line(x + pad, y, content_w, value, value_br.Get(), dt::kFontSizeXsDip)
                    + gap - dt::kSpaceXsDip * dpi_s;
                continue;
            }

            int cur_idx = out_clickable ? static_cast<int>(out_clickable->size()) : -1;
            float y1 = draw_text_line(x + pad, y, lw,      label, label_br.Get(), dt::kFontSizeXsDip);
            float vw = 0;
            float y2 = draw_text_line(x + pad + lw + cgap, y, val_w,
                                      value, value_br.Get(), dt::kFontSizeXsDip, &vw, 10);
            if (out_clickable && !value.empty()) {
                D2D1_RECT_F cr = {x + pad + lw + cgap, y, x + pad + lw + cgap + val_w, y2};
                out_clickable->push_back({cr, value, label});
                if (sel_idx == cur_idx) {
                    float hw = std::min(vw + dt::kSpaceSmDip * dpi_s, val_w);
                    float hx = x + pad + lw + cgap - dt::kSpaceXsDip * dpi_s;
                    D2D1_RECT_F hr = {hx, y, hx + hw + dt::kSpaceXsDip * dpi_s, y2};
                    D2D1_ROUNDED_RECT hrr = {
                        hr, dt::kPanelSelectionCornerRadiusDip * dpi_s,
                        dt::kPanelSelectionCornerRadiusDip * dpi_s};
                    auto sel_br = get_solid_brush(dt::d2d(dt::kColorPanelSelection));
                    m_d2d_context->FillRoundedRectangle(&hrr, sel_br.Get());
                    draw_text_line(x + pad + lw + cgap, y, val_w, value, value_br.Get(), dt::kFontSizeXsDip, nullptr, 10);
                }
            }
            y = std::max(y1, y2) + gap - dt::kSpaceXsDip * dpi_s;
        }
    }

    // Toast notification
    if (toast && !toast->empty()) {
        PanelToastLayoutInput layout_input;
        layout_input.panel_x = x;
        layout_input.panel_y = y_off;
        layout_input.panel_width = w;
        layout_input.panel_height = h;
        layout_input.dpi = m_dpi_y;
        const auto constraints = calculate_panel_toast_layout(layout_input);

        if (constraints.maximum_text_width > 0.0f
            && constraints.maximum_text_height > 0.0f) {
            auto toast_format = get_text_format(dt::kFontFamilyUi,
                dt::kFontSizeSmDip * dpi_s, dt::kFontWeightNormal,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                L"zh-CN");
            if (toast_format) {
                toast_format->SetWordWrapping(
                    DWRITE_WORD_WRAPPING_EMERGENCY_BREAK);
                auto create_layout = [&](float max_width, float max_height) {
                    ComPtr<IDWriteTextLayout> layout;
                    m_dwrite_factory->CreateTextLayout(toast->c_str(),
                        static_cast<uint32_t>(toast->size()), toast_format.Get(),
                        max_width, max_height, &layout);
                    return layout;
                };

                auto unwrapped_layout = create_layout(
                    100000.0f * dpi_s, 100000.0f * dpi_s);
                if (unwrapped_layout) {
                    unwrapped_layout->SetWordWrapping(
                        DWRITE_WORD_WRAPPING_NO_WRAP);
                }
                DWRITE_TEXT_METRICS unwrapped_metrics = {};
                if (unwrapped_layout && SUCCEEDED(
                        unwrapped_layout->GetMetrics(&unwrapped_metrics))) {
                    layout_input.measured_text_width =
                        unwrapped_metrics.widthIncludingTrailingWhitespace;
                }

                const auto width_layout =
                    calculate_panel_toast_layout(layout_input);
                const float text_width = std::max(1.0f,
                    width_layout.text_bounds.right
                        - width_layout.text_bounds.left);
                auto text_layout = create_layout(text_width,
                    constraints.maximum_text_height);
                DWRITE_TEXT_METRICS metrics = {};
                if (text_layout && SUCCEEDED(text_layout->GetMetrics(&metrics))) {
                    layout_input.measured_text_height = metrics.height;
                    layout_input.line_count = metrics.lineCount;
                    const auto layout =
                        calculate_panel_toast_layout(layout_input);

                    text_layout->SetMaxWidth(std::max(1.0f,
                        layout.text_bounds.right - layout.text_bounds.left));
                    text_layout->SetMaxHeight(std::max(1.0f,
                        layout.text_bounds.bottom - layout.text_bounds.top));
                    text_layout->SetTextAlignment(layout.single_line
                        ? DWRITE_TEXT_ALIGNMENT_CENTER
                        : DWRITE_TEXT_ALIGNMENT_LEADING);

                    auto toast_bg = get_solid_brush(
                        dt::d2d(dt::kColorPanelToastBg));
                    auto toast_txt = get_solid_brush(
                        dt::d2d(dt::kColorPanelToastText));
                    if (toast_bg && toast_txt) {
                        const D2D1_RECT_F bounds = {
                            layout.bounds.left, layout.bounds.top,
                            layout.bounds.right, layout.bounds.bottom};
                        const D2D1_ROUNDED_RECT rounded = {
                            bounds, layout.corner_radius, layout.corner_radius};
                        m_d2d_context->FillRoundedRectangle(
                            &rounded, toast_bg.Get());

                        const D2D1_RECT_F text_bounds = {
                            layout.text_bounds.left, layout.text_bounds.top,
                            layout.text_bounds.right, layout.text_bounds.bottom};
                        m_d2d_context->PushAxisAlignedClip(
                            &text_bounds, D2D1_ANTIALIAS_MODE_ALIASED);
                        m_d2d_context->DrawTextLayout(
                            {text_bounds.left, text_bounds.top},
                            text_layout.Get(), toast_txt.Get());
                        m_d2d_context->PopAxisAlignedClip();
                    }
                }
            }
        }
    }

    // Pop panel clip
    m_d2d_context->PopAxisAlignedClip();

    // Scrollbar on panel right edge
    float total_h = y - y0 + scroll_y + pad;  // extra pad for bottom margin
    if (total_h > h) {
        float sb_w = dt::kSpace6Dip * dpi_s;
        draw_scrollbar(x + w - sb_w - 2.0f * dpi_s, y_off, sb_w, h, total_h, h, scroll_y);
    }
    return total_h;
}

void Renderer::draw_scrollbar(float x, float y, float w, float h,
    float total, float view, float pos, bool active)
{
    if (!m_d2d_context || total <= 0 || view >= total) return;

    float ratio = view / total;
    float thumb_h = std::max(dt::kScrollbarMinThumbPx, h * ratio);
    float range = total - view;
    float pct = (range > 0) ? std::min(1.0f, pos / range) : 0.0f;
    float thumb_y = y + (h - thumb_h) * pct;

    float inner_w  = active
        ? w - dt::kScrollbarActiveInsetPx
        : w - dt::kScrollbarIdleInsetPx;  // wider when active
    float offset_x = (w - inner_w) / 2.0f;

    auto thumb = get_solid_brush(
        dt::d2d(active ? dt::kColorScrollbarThumbActive
                       : dt::kColorScrollbarThumbIdle));

    D2D1_RECT_F tr = {x + offset_x, thumb_y, x + offset_x + inner_w, thumb_y + thumb_h};
    float radius = inner_w * dt::kScrollbarThumbRadiusFraction;
    D2D1_ROUNDED_RECT rr = {tr, radius, radius};
    m_d2d_context->FillRoundedRectangle(&rr, thumb.Get());
}

void Renderer::draw_filmstrip(float x, float y, float w, float h,
    std::span<const FilmstripRenderItem> items,
    bool left_overflow, bool right_overflow,
    float anim_t)
{
    if (!m_d2d_context) return;

    const float dpi_s = m_dpi_y / 96.0f;
    const float radius = layout::kThumbCornerRadiusDip * dpi_s;

    // Strip background: vertical gradient instead of flat #141416.
    // Bottom (0%..grad_start) is solid black at 50% opacity; from
    // grad_start (50% or 67% height) to the top (100%) it fades to 0%.
    // The alpha falloff uses a smoothstep "interpolator" so the fade is
    // eased, not linear — gradient stops are plain data, so we bake the
    // easing into per-stop alphas (no D2D gradient animation API exists).
    if (!m_filmstrip_bg_gradient || m_filmstrip_bg_dpi != m_dpi_y
        || m_filmstrip_bg_width != w) {
        m_filmstrip_bg_gradient.Reset();
        m_filmstrip_bg_dpi = m_dpi_y;
        m_filmstrip_bg_width = w;
        constexpr float kGradStart = 0.5f;  // fade begins at 50% height
        std::array<D2D1_GRADIENT_STOP, 9> stops{};
        constexpr int kFadeSegs = 7;  // eased segments from start..top
        stops[0] = {0.0f, dt::d2d(dt::with_alpha(dt::kColorBlack, 0.5f))};
        stops[1] = {kGradStart, dt::d2d(dt::with_alpha(dt::kColorBlack, 0.5f))};
        for (int i = 0; i < kFadeSegs; ++i) {
            const float t = static_cast<float>(i + 1) / kFadeSegs;  // 1/7..1
            const float eased = t * t * (3.0f - 2.0f * t);  // smoothstep
            const float pos = kGradStart + (1.0f - kGradStart) * t;
            stops[2 + i] = {pos,
                dt::d2d(dt::with_alpha(dt::kColorBlack,
                    0.5f * (1.0f - eased)))};
        }
        ComPtr<ID2D1GradientStopCollection> coll;
        if (SUCCEEDED(m_d2d_context->CreateGradientStopCollection(
                stops.data(), static_cast<UINT32>(stops.size()), &coll))) {
            m_d2d_context->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(x, y + h),  // bottom = 50% black
                    D2D1::Point2F(x, y)),     // top = transparent
                coll.Get(), &m_filmstrip_bg_gradient);
        }
    }
    if (m_filmstrip_bg_gradient) {
        m_d2d_context->FillRectangle(
            D2D1::RectF(x, y, x + w, y + h), m_filmstrip_bg_gradient.Get());
    }

    // Clip slightly larger than the strip so the current item's selection
    // border and its 1.25x top overhang (bottom-aligned magnification
    // grows upward by ~6 DIP) are not cut off.
    const float clip_sw = dt::kSpace10Dip * dpi_s;
    D2D1_RECT_F strip_clip = {x, y - clip_sw, x + w, y + h + clip_sw};
    m_d2d_context->PushAxisAlignedClip(&strip_clip, D2D1_ANTIALIAS_MODE_ALIASED);

    // True alpha mask: the strip content's opacity follows a horizontal
    // gradient (transparent at both edges -> opaque in the middle) applied
    // via D2D1_LAYER_PARAMETERS1.opacityBrush, so thumbnails genuinely
    // fade out at the edges instead of being covered by an overlay block.
    if (!m_filmstrip_mask_gradient || m_filmstrip_mask_dpi != m_dpi_y
        || m_filmstrip_mask_width != w) {
        m_filmstrip_mask_gradient.Reset();
        m_filmstrip_mask_dpi = m_dpi_y;
        m_filmstrip_mask_width = w;
        constexpr float kFade = 0.06f;  // 6% of strip width per edge
        const D2D1_GRADIENT_STOP stops[4] = {
            {0.00f, dt::d2d(dt::with_alpha(dt::kColorBlack, 0.0f))},
            {kFade, dt::d2d(dt::with_alpha(dt::kColorBlack, 1.0f))},
            {1.0f - kFade, dt::d2d(dt::with_alpha(dt::kColorBlack, 1.0f))},
            {1.00f, dt::d2d(dt::with_alpha(dt::kColorBlack, 0.0f))},
        };
        ComPtr<ID2D1GradientStopCollection> coll;
        if (SUCCEEDED(m_d2d_context->CreateGradientStopCollection(
                stops, 4, &coll))) {
            m_d2d_context->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F(x, y), D2D1::Point2F(x + w, y)),
                coll.Get(), &m_filmstrip_mask_gradient);
        }
    }
    ComPtr<ID2D1Layer> mask_layer;
    const bool mask_active = m_filmstrip_mask_gradient
        && (m_filmstrip_mask_layer
            || SUCCEEDED(m_d2d_context->CreateLayer(&m_filmstrip_mask_layer)));
    if (mask_active) {
        const D2D1_LAYER_PARAMETERS1 lp = D2D1::LayerParameters1(
            D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_ALIASED,
            D2D1::IdentityMatrix(), 1.0f, m_filmstrip_mask_gradient.Get(),
            D2D1_LAYER_OPTIONS1_NONE);
        m_d2d_context->PushLayer(lp, m_filmstrip_mask_layer.Get());
    }

    // Draw normal items first; magnified items (the previous one during
    // its shrink animation, then the current one) are drawn last so they
    // float above neighbors. The selection border fades in on the new
    // item and fades out on the previous one (alpha from zoom progress).
    int current_index = -1;
    int shrink_index = -1;
    for (std::size_t i = 0; i < items.size(); ++i) {
        const FilmstripRenderItem& item = items[i];
        if (item.current) { current_index = static_cast<int>(i); continue; }
        if (item.zoom > 1.0f) { shrink_index = static_cast<int>(i); continue; }
        if (item.width <= 0.0f || item.height <= 0.0f) continue;
        const D2D1_RECT_F rc = {
            x + item.left, y + item.top,
            x + item.left + item.width, y + item.top + item.height};

        if (item.bitmap) {
            // Rounded-corner masked bitmap (same pattern as grid thumbnails).
            auto geo = get_rounded_geometry(item.width, item.height, radius);
            if (geo) {
                m_d2d_context->PushLayer(
                    D2D1::LayerParameters(D2D1::InfiniteRect(), geo.Get(),
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                        D2D1::Matrix3x2F::Translation(rc.left, rc.top),
                        1.0f, nullptr,
                        D2D1_LAYER_OPTIONS_NONE),
                    nullptr);
                m_d2d_context->DrawBitmap(item.bitmap, &rc, 1.0f,
                    D2D1_INTERPOLATION_MODE_LINEAR, nullptr);
                m_d2d_context->PopLayer();
            } else {
                m_d2d_context->DrawBitmap(item.bitmap, &rc, 1.0f,
                    D2D1_INTERPOLATION_MODE_LINEAR, nullptr);
            }
        } else {
            // Skeleton placeholder with the image's dominant color.
            D2D1_COLOR_F fill = item.placeholder_color;
            if (std::max({fill.r, fill.g, fill.b}) < 0.18f)
                fill = dt::d2d(dt::kColorPlaceholderMin);
            D2D1_ROUNDED_RECT rr = {rc, radius, radius};
            auto brush = get_solid_brush(fill);
            m_d2d_context->FillRoundedRectangle(&rr, brush.Get());
        }
    }

    // Magnified items float above the strip. Each carries its own
    // selection border: the old item's border fades out (alpha 1->0)
    // while it shrinks back to 1.0x, the new item's border fades in
    // (alpha 0->1) while it grows to 1.25x. Border alpha uses a CUBIC
    // ease (et^3): early in the transition the new border is nearly
    // invisible, so the eye keeps the focus on the old item at center;
    // the focus hands over smoothly as the strip scrolls one slot — no
    // "border jumps right, then scrolls back" artifact.
    const auto draw_magnified = [&](int item_index, float dpi_s,
        float border_alpha) {
        if (item_index < 0) return;
        const FilmstripRenderItem& item = items[static_cast<std::size_t>(item_index)];
        if (item.width <= 0.0f || item.height <= 0.0f) return;
        const D2D1_RECT_F rc = {
            x + item.left, y + item.top,
            x + item.left + item.width, y + item.top + item.height};
        if (item.bitmap) {
            auto geo = get_rounded_geometry(item.width, item.height, radius);
            if (geo) {
                m_d2d_context->PushLayer(
                    D2D1::LayerParameters(D2D1::InfiniteRect(), geo.Get(),
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                        D2D1::Matrix3x2F::Translation(rc.left, rc.top),
                        1.0f, nullptr,
                        D2D1_LAYER_OPTIONS_NONE),
                    nullptr);
                m_d2d_context->DrawBitmap(item.bitmap, &rc, 1.0f,
                    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
                m_d2d_context->PopLayer();
            } else {
                m_d2d_context->DrawBitmap(item.bitmap, &rc, 1.0f,
                    D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
            }
        } else {
            D2D1_COLOR_F fill = item.placeholder_color;
            if (std::max({fill.r, fill.g, fill.b}) < 0.18f)
                fill = dt::d2d(dt::kColorPlaceholderMin);
            D2D1_ROUNDED_RECT rr = {rc, radius, radius};
            auto brush = get_solid_brush(fill);
            m_d2d_context->FillRoundedRectangle(&rr, brush.Get());
        }
        if (border_alpha > 0.0f) {
            const float sw = layout::kFilmstripBorderDip * dpi_s;
            // Stroke centered half-inside the bitmap edge: the border's
            // OUTER edge lands exactly on the thumbnail edge, so it never
            // invades the fixed gap toward the neighbor.
            draw_selection_border(D2D1::RectF(
                rc.left - sw * 0.5f, rc.top - sw * 0.5f,
                rc.right + sw * 0.5f, rc.bottom + sw * 0.5f),
                border_alpha);
        }
    };

    const float et = 1.0f - (1.0f - anim_t) * (1.0f - anim_t)
        * (1.0f - anim_t) * (1.0f - anim_t);  // quartic (scroll pace)
    const float border_ease = et * et * et;   // cubic (focus handover)

    // Previous item (shrink animation) floats above the strip, then the
    // current magnified item is drawn last.
    draw_magnified(shrink_index, dpi_s, 1.0f - border_ease);
    draw_magnified(current_index, dpi_s, border_ease);

    // Pop the alpha-mask layer: thumbnails at the strip edges are now
    // transparent (faded by the gradient), not covered by an overlay.
    if (mask_active) {
        m_d2d_context->PopLayer();
    }

    // Edge arrows (drawn outside the mask so they never fade out).
    const float arrow_zone = layout::kFilmstripArrowZoneDip * dpi_s;
    if (left_overflow) {
        draw_filmstrip_arrow(x + arrow_zone * 0.5f, y + h * 0.5f,
            L"\u25C0", dpi_s);
    }
    if (right_overflow) {
        draw_filmstrip_arrow(x + w - arrow_zone * 0.5f, y + h * 0.5f,
            L"\u25B6", dpi_s);
    }

    m_d2d_context->PopAxisAlignedClip();
}

void Renderer::draw_filmstrip_arrow(float cx, float cy, const wchar_t* glyph,
    float dpi_scale)
{
    if (!m_d2d_context || !m_dwrite_factory) return;
    auto tf = get_text_format(dt::kFontFamilyUi,
        dt::kFontSizeMdDip * dpi_scale, dt::kFontWeightNormal,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, L"en-US");
    if (!tf) return;
    tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(m_dwrite_factory->CreateTextLayout(
            glyph, 1, tf.Get(), dt::kSpaceXlDip * dpi_scale,
            dt::kSpaceXlDip * dpi_scale, &layout))) {
        return;
    }
    auto brush = get_solid_brush(dt::d2d(dt::kColorFilmstripArrow));
    m_d2d_context->DrawTextLayout(
        D2D1::Point2F(cx - dt::kSpaceMdDip * dpi_scale,
            cy - dt::kSpaceMdDip * dpi_scale),
        layout.Get(), brush.Get());
}

float Renderer::draw_text_line(float x, float y, float w,
    const std::wstring& text, ID2D1SolidColorBrush* brush,
    float font_size, float* out_width, int max_lines)
{
    if (!m_dwrite_factory || !m_d2d_context || text.empty()) return y + 20;

    IDWriteTextFormat* fmt = m_text_format.Get();
    ComPtr<IDWriteTextFormat> sized_fmt;
    if (font_size > 0.0f && m_dwrite_factory) {
        sized_fmt = get_text_format(dt::kFontFamilyUi,
            font_size * m_dpi_y / 96.0f, dt::kFontWeightNormal,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, L"en-US");
        if (sized_fmt) fmt = sized_fmt.Get();
    }

    ComPtr<IDWriteTextLayout> layout;
    HRESULT hr = m_dwrite_factory->CreateTextLayout(
        text.c_str(), static_cast<uint32_t>(text.size()),
        fmt, w, max_lines > 0 ? font_size * m_dpi_y / 96.0f * 1.4f * max_lines : 200.0f, &layout);
    if (FAILED(hr)) return y + 20;

    // Set trimming before getting metrics
    if (max_lines > 0) {
        ComPtr<IDWriteTextLayout1> layout1;
        layout.As(&layout1);
        if (layout1) {
            DWRITE_TRIMMING trimming = { DWRITE_TRIMMING_GRANULARITY_CHARACTER };
            ComPtr<IDWriteInlineObject> ellipsis;
            m_dwrite_factory->CreateEllipsisTrimmingSign(fmt, &ellipsis);
            layout1->SetTrimming(&trimming, ellipsis.Get());
        }
    }

    DWRITE_TEXT_METRICS metrics;
    layout->GetMetrics(&metrics);
    if (out_width) *out_width = metrics.widthIncludingTrailingWhitespace;
    float h = metrics.height;
    if (max_lines > 0) {
        float max_h = max_lines * font_size * m_dpi_y / 96.0f * 1.44f;
        if (h > max_h) h = max_h;
    }

    D2D1_POINT_2F origin = {x, y};
    m_d2d_context->DrawTextLayout(origin, layout.Get(), brush);
    return y + h + 4;  // 4px gap
}

float Renderer::measure_text(const std::wstring& text, float font_size) {
    if (!m_dwrite_factory) return 0;
    auto tf = get_text_format(dt::kFontFamilyUi, font_size,
        dt::kFontWeightNormal, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, L"en-US");
    ComPtr<IDWriteTextLayout> layout;
    m_dwrite_factory->CreateTextLayout(text.c_str(), static_cast<uint32_t>(text.size()),
        tf.Get(), 2000.0f, 30.0f, &layout);
    DWRITE_TEXT_METRICS m;
    layout->GetMetrics(&m);
    return m.width;
}

void Renderer::draw_toolbar(float w, const std::vector<std::wstring>& items, int active_idx, float y) {
    if (!m_d2d_context || !m_dwrite_factory) return;

    float h = dt::dip(dt::kSize28Dip, m_dpi_y);
    auto bg = get_solid_brush(dt::d2d(dt::kColorToolbarBg));
    auto text_brush = get_solid_brush(dt::d2d(dt::kColorToolbarText));
    auto hover_bg = get_solid_brush(dt::d2d(dt::kColorToolbarHoverBg));

    D2D1_RECT_F rc = {0, y, w, y + h};
    m_d2d_context->FillRectangle(&rc, bg.Get());

    auto tf = get_text_format(dt::kFontFamilyUi,
        dt::dip(dt::kFontSizeLgDip, m_dpi_y), dt::kFontWeightNormal,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, L"en-US");

    float x = dt::kToolbarPadXPx;
    for (int i = 0; i < static_cast<int>(items.size()); ++i) {
        ComPtr<IDWriteTextLayout> layout;
        m_dwrite_factory->CreateTextLayout(items[i].c_str(),
            static_cast<uint32_t>(items[i].size()), tf.Get(), 200.0f, 30.0f, &layout);
        DWRITE_TEXT_METRICS m;
        layout->GetMetrics(&m);
        float iw = m.width + 2.0f * dt::kToolbarPadXPx;

        if (i == active_idx) {
            D2D1_RECT_F hr = {x, y + dt::kToolbarItemInsetYPx,
                               x + iw, y + h - dt::kToolbarItemInsetYPx};
            m_d2d_context->FillRectangle(&hr, hover_bg.Get());
        }

        D2D1_POINT_2F pt = {x + dt::kToolbarPadXPx, y + (h - m.height) / 2.0f};
        m_d2d_context->DrawTextLayout(pt, layout.Get(), text_brush.Get());
        x += iw;
    }
}

void Renderer::draw_title_bar(float w, int hover_btn, int press_btn,
    const std::vector<std::wstring>& menu_items, int active_menu)
{
    if (!m_d2d_context || !m_dwrite_factory) return;

    float dpi_s = m_dpi_y / 96.0f;
    float h = std::max(static_cast<float>(layout::kTitleBarHeightDip) * dpi_s, GetSystemMetrics(SM_CYCAPTION) + 2.0f);
    float pad = layout::kTitleBarPadDip * dpi_s;
    float btn_w = layout::kTitleBarButtonWidthDip * dpi_s;

    // ── No background — fully immersive, clip keeps content below ──

    // ── Brushes ──
    auto title_br = get_solid_brush(dt::d2d(dt::kColorTitleText));
    auto menu_br = get_solid_brush(dt::d2d(dt::kColorTitleMenuText));
    auto menu_hover_bg = get_solid_brush(dt::d2d(dt::kColorTitleMenuHoverBg));

    // ── Title text (left) ──
    float fs = layout::kTitleBarMenuFontSizeDip * dpi_s;
    auto tf = get_text_format(dt::kFontFamilyUi, fs, dt::kFontWeightBold,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, L"en-US");
    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    float title_w = layout::kTitleBarTitleWidthDip * dpi_s;
    D2D1_RECT_F trc = {pad, 0, pad + title_w, h};
    m_d2d_context->DrawText(L"MinView", 7, tf.Get(), &trc, title_br.Get());

    // ── Menu items (after title) ──
    auto mtf = get_text_format(dt::kFontFamilyUi, fs,
        dt::kFontWeightNormal, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, L"en-US");
    float mx = pad + title_w + layout::kTitleBarTitleGapDip * dpi_s;
    for (int i = 0; i < static_cast<int>(menu_items.size()); ++i) {
        ComPtr<IDWriteTextLayout> layout;
        m_dwrite_factory->CreateTextLayout(menu_items[i].c_str(),
            static_cast<uint32_t>(menu_items[i].size()), mtf.Get(), 200.0f, h, &layout);
        DWRITE_TEXT_METRICS m;
        layout->GetMetrics(&m);
        float mw = m.width + layout::kTitleBarMenuPadDip * dpi_s;  // padding each side

        // Hover highlight
        if (i == active_menu) {
            D2D1_RECT_F hr = {
                mx - dt::kSpaceXsDip * dpi_s,
                dt::kTitleBarMenuHoverInsetYDip * dpi_s,
                mx + mw - dt::kSpaceXsDip * dpi_s,
                h - dt::kTitleBarMenuHoverInsetYDip * dpi_s};
            m_d2d_context->FillRoundedRectangle(
                D2D1::RoundedRect(
                    hr, dt::kTitleBarMenuHoverRadiusDip * dpi_s,
                    dt::kTitleBarMenuHoverRadiusDip * dpi_s),
                menu_hover_bg.Get());
        }

        D2D1_POINT_2F pt = {mx + dt::kSpaceXsDip * dpi_s, (h - m.height) * 0.5f};
        m_d2d_context->DrawTextLayout(pt, layout.Get(), menu_br.Get());
        mx += mw;
    }

    // ── Window buttons (right) ──
    auto draw_btn = [&](float bx, int id, const wchar_t* sym) {
        bool hover = (hover_btn == id);
        bool press = (press_btn == id);
        bool is_close = (id == 2);
        if (hover) {
            float a = press ? 0.7f : 0.35f;
            auto bb = get_solid_brush(dt::d2d(dt::with_alpha(
                is_close ? dt::kColorWindowButtonCloseHover
                         : dt::kColorWindowButtonHover,
                a)));
            D2D1_RECT_F br = {bx, 0, bx + btn_w, h};
            m_d2d_context->FillRectangle(&br, bb.Get());
        }
        auto sb = get_solid_brush(dt::d2d(dt::kColorWindowButtonSymbol));
        float sfs = dt::kFontSizeXsDip * dpi_s;
        auto stf = get_text_format(dt::kFontFamilySymbols, sfs,
            dt::kFontWeightNormal, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, L"en-US");
        stf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        stf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        D2D1_RECT_F sr = {bx, 0, bx + btn_w, h};
        m_d2d_context->DrawText(sym, 1, stf.Get(), &sr, sb.Get());
    };
    draw_btn(w - btn_w, 2, L"\u2715");                // close
    draw_btn(w - btn_w * 2, 1, L"\u25A1");            // maximize
    draw_btn(w - btn_w * 3, 0, L"\u2014");            // minimize
}

void Renderer::draw_fade_overlay(float t, bool forward) {
    if (!m_d2d_context) return;
    // The big-image background (a solid color) fades over the grid:
    // entry 0 -> 100% covers the grid snapshot, exit 100 -> 0% reveals
    // the grid underneath. Three transforms total — translation, scale,
    // background opacity — nothing else.
    const float s = std::clamp(t, 0.0f, 1.0f);
    // Same FLIP ease-in-out curve for both directions; the exit is the
    // exact time-mirror of the entry (background crossfade per recording).
    const float et = transition_ease(s);
    const float alpha = forward ? et : (1.0f - et);
    if (alpha <= 0.0f) return;
    auto br = get_solid_brush(
        dt::d2d(dt::with_alpha(dt::kColorCanvas, std::clamp(alpha, 0.0f, 1.0f))));
    D2D1_RECT_F rc = {0, 0, static_cast<float>(m_target_size.width),
                      static_cast<float>(m_target_size.height)};
    m_d2d_context->FillRectangle(&rc, br.Get());
}

void Renderer::draw_fullscreen_bitmap(ID2D1Bitmap1* bmp) {
    if (!m_d2d_context || !bmp) return;
    const D2D1_RECT_F rc = {0.0f, 0.0f,
        static_cast<float>(m_target_size.width),
        static_cast<float>(m_target_size.height)};
    m_d2d_context->DrawBitmap(bmp, &rc, 1.0f,
        D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, nullptr);
}

// Shared FLIP transition geometry: uniform scale + center translation,
// CSS ease-in-out cubic-bezier(0.42,0,0.58,1); exit replays the same
// curve forward, which is the exact time-mirror of the entry.
static D2D1_RECT_F transition_interpolated_rect(
    D2D1_RECT_F src, D2D1_RECT_F dst, float t, float& out_et,
    bool exit_curve = false) {
    const float s = std::clamp(t, 0.0f, 1.0f);
    const float et = exit_curve ? transition_ease_exit(s) : transition_ease(s);
    out_et = et;
    const float dst_w = dst.right - dst.left, dst_h = dst.bottom - dst.top;
    const float aspect = dst_w / dst_h;
    const float src_cx = (src.left + src.right) * 0.5f;
    const float src_cy = (src.top + src.bottom) * 0.5f;
    const float dst_cx = (dst.left + dst.right) * 0.5f;
    const float dst_cy = (dst.top + dst.bottom) * 0.5f;
    const float src_area = (src.right - src.left) * (src.bottom - src.top);
    const float src_h = std::sqrt(src_area / aspect);
    const float src_w = src_h * aspect;
    const float cur_w = src_w + (dst_w - src_w) * et;
    const float cur_h = src_h + (dst_h - src_h) * et;
    const float cx = src_cx + (dst_cx - src_cx) * et;
    const float cy = src_cy + (dst_cy - src_cy) * et;
    return D2D1_RECT_F{cx - cur_w * 0.5f, cy - cur_h * 0.5f,
                       cx + cur_w * 0.5f, cy + cur_h * 0.5f};
}

void Renderer::draw_anim_thumb(ID2D1Bitmap1* bmp, D2D1_RECT_F src, D2D1_RECT_F dst, float t) {
    // Content handoff: fade the zooming layer out over the final third so
    // the full image underneath takes over without a pop.
    const float s = std::clamp(t, 0.0f, 1.0f);
    const float fade = s < 0.68f
        ? 1.0f : std::max(0.0f, 1.0f - (s - 0.68f) / 0.32f);
    draw_anim_thumb_faded(bmp, src, dst, t, fade, false);
}

void Renderer::draw_anim_thumb_faded(ID2D1Bitmap1* bmp, D2D1_RECT_F src,
    D2D1_RECT_F dst, float t, float alpha, bool exit_curve) {
    if (!m_d2d_context || !bmp) return;
    if (src.right <= src.left || src.bottom <= src.top) return;
    if (dst.right <= dst.left || dst.bottom <= dst.top) return;
    float et = 0.0f;
    const D2D1_RECT_F rc =
        transition_interpolated_rect(src, dst, t, et, exit_curve);
    m_d2d_context->DrawBitmap(bmp, &rc, std::clamp(alpha, 0.0f, 1.0f),
        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
}

void Renderer::draw_anim_image(ID2D1Bitmap1* image, D2D1_RECT_F src, D2D1_RECT_F dst, float t) {
    if (!m_d2d_context || !image) return;
    if (src.right <= src.left || src.bottom <= src.top) return;
    if (dst.right <= dst.left || dst.bottom <= dst.top) return;
    float et = 0.0f;
    const D2D1_RECT_F rc = transition_interpolated_rect(src, dst, t, et);
    // Aspect-fit the full image inside the interpolated rect (it matches
    // the destination aspect at t=1, so the fit converges to the final
    // layout exactly).
    const D2D1_SIZE_F size = image->GetSize();
    if (size.width <= 0.0f || size.height <= 0.0f) return;
    const float rw = rc.right - rc.left, rh = rc.bottom - rc.top;
    const float scale = std::min(rw / size.width, rh / size.height);
    const float dw = size.width * scale, dh = size.height * scale;
    const D2D1_RECT_F dest = D2D1::RectF(
        (rc.left + rc.right - dw) * 0.5f, (rc.top + rc.bottom - dh) * 0.5f,
        (rc.left + rc.right + dw) * 0.5f, (rc.top + rc.bottom + dh) * 0.5f);
    m_d2d_context->DrawBitmap(image, &dest, 1.0f,
        D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr);
}

void Renderer::push_clip_below(float y) {
    if (!m_d2d_context) return;
    D2D1_RECT_F clip = {0, y, static_cast<float>(m_target_size.width),
                        static_cast<float>(m_target_size.height)};
    m_d2d_context->PushAxisAlignedClip(&clip, D2D1_ANTIALIAS_MODE_ALIASED);
}

void Renderer::push_clip_rect(const D2D1_RECT_F& rc) {
    if (!m_d2d_context) return;
    m_d2d_context->PushAxisAlignedClip(&rc, D2D1_ANTIALIAS_MODE_ALIASED);
}

void Renderer::pop_clip() {
    if (!m_d2d_context) return;
    m_d2d_context->PopAxisAlignedClip();
}

// ── Left navigation panel + breadcrumb (Issue #5 P2) ────────

void Renderer::draw_breadcrumb(const NavBreadcrumbRenderInput& input) {
    if (!m_d2d_context || !m_dwrite_factory) return;
    if (!input.layout || !input.segments) return;
    const float dpi_s = input.dpi_scale > 0.0f ? input.dpi_scale : m_dpi_y / 96.0f;
    const float fs = layout::kNavFontSizeDip;

    auto text_br = get_solid_brush(dt::d2d(dt::kColorTitleText));
    auto hover_br = get_solid_brush(dt::d2d(dt::kColorBreadcrumbHover));
    auto dim_br = get_solid_brush(dt::d2d(dt::kColorTextDim));
    auto line_br = get_solid_brush(dt::d2d(dt::kColorBreadcrumbLine));

    for (int i = 0; i < static_cast<int>(input.layout->items.size()); ++i) {
        const auto& item = input.layout->items[i];
        if (item.segment_index < 0 && !item.ellipsis) continue;
        const std::wstring text = item.ellipsis
            ? L"\u2026"
            : (*input.segments)[static_cast<size_t>(item.segment_index)];
        if (text.empty()) continue;
        ID2D1SolidColorBrush* brush = item.ellipsis
            ? dim_br.Get()
            : (i == input.hover_item ? hover_br.Get() : text_br.Get());
        const float tw = std::max(1.0f, item.width + dt::kSpaceXsDip * dpi_s);
        const float th = label_height(text, tw, fs, 1);
        draw_text_line(item.x, input.y + (input.height - th) * 0.5f,
            tw, text, brush, fs, nullptr, 1);
    }

    const float line_y = input.y + input.height - 1.0f;
    m_d2d_context->DrawLine(
        D2D1::Point2F(input.x, line_y),
        D2D1::Point2F(input.x + std::max(0.0f, input.width), line_y),
        line_br.Get(), 1.0f);
}

void Renderer::draw_nav_panel(const NavPanelRenderInput& input) {
    if (!m_d2d_context || !m_dwrite_factory) return;
    const auto& g = input.geometry;
    const float dpi_s = input.dpi_scale > 0.0f ? input.dpi_scale : m_dpi_y / 96.0f;
    const float pad = layout::kNavPadDip * dpi_s;
    const float fs = layout::kNavFontSizeDip;
    const float small_fs = layout::kNavSmallFontSizeDip;

    auto bg_br = get_solid_brush(dt::d2d(dt::kColorToolbarBg));       // #1c1c21
    auto line_br = get_solid_brush(dt::d2d(dt::kColorNavLine));     // #292931
    auto text_br = get_solid_brush(dt::d2d(dt::kColorTitleText));
    auto dim_br = get_solid_brush(dt::d2d(dt::kColorNavDim));      // #80808c
    auto bright_br = get_solid_brush(dt::d2d(dt::kColorNavBright));
    auto hover_bg = get_solid_brush(dt::d2d(dt::kColorNavHoverBg));    // #26262e
    auto sel_bg = get_solid_brush(dt::d2d(dt::kColorToolbarHoverBg));      // #2e2e38
    auto accent_br = get_solid_brush(dt::d2d(dt::kColorNavAccent));   // #4A90E2
    auto badge_br = get_solid_brush(dt::d2d(dt::kColorNavBadge));
    auto error_br = get_solid_brush(dt::d2d(dt::kColorNavError));    // #b08080

    // Panel background + right edge
    m_d2d_context->FillRectangle(
        D2D1::RectF(g.x, g.y, g.x + g.w, g.y + g.h), bg_br.Get());
    m_d2d_context->DrawLine(
        D2D1::Point2F(g.x + g.w - 1.0f, g.y),
        D2D1::Point2F(g.x + g.w - 1.0f, g.y + g.h), line_br.Get(), 1.0f);

    // Breadcrumb strip
    NavBreadcrumbRenderInput bc;
    bc.x = g.x;
    bc.y = g.breadcrumb_y;
    bc.width = g.w;
    bc.height = g.breadcrumb_h;
    bc.layout = input.breadcrumb;
    bc.segments = input.segments;
    bc.hover_item = input.breadcrumb_hover;
    bc.dpi_scale = dpi_s;
    draw_breadcrumb(bc);

    // Tab row: 目录 | 收藏
    const std::wstring tab_dirs = L"\u76EE\u5F55";
    const std::wstring tab_fav = L"\u6536\u85CF";
    const float w_dirs = measure_text(tab_dirs, fs * dpi_s);
    const float w_fav = measure_text(tab_fav, fs * dpi_s);
    float tx = g.x + pad;
    const auto draw_tab = [&](const std::wstring& label, float label_w,
                              float x, bool active) {
        ID2D1SolidColorBrush* brush = active ? bright_br.Get() : dim_br.Get();
        const float th = label_height(label, label_w + 4.0f, fs, 1);
        draw_text_line(x, g.tabs_y + (g.tabs_h - th) * 0.5f,
            label_w + 4.0f, label, brush, fs, nullptr, 1);
        if (active) {
            m_d2d_context->FillRectangle(
                D2D1::RectF(x, g.tabs_y + g.tabs_h - 2.0f * dpi_s,
                    x + label_w, g.tabs_y + g.tabs_h),
                accent_br.Get());
        }
    };
    draw_tab(tab_dirs, w_dirs, tx, input.tab == NavPanelTab::Directories);
    tx += w_dirs + dt::kSpaceLgDip * dpi_s;
    draw_tab(tab_fav, w_fav, tx, input.tab == NavPanelTab::Favorites);
    m_d2d_context->DrawLine(
        D2D1::Point2F(g.x, g.tabs_y + g.tabs_h),
        D2D1::Point2F(g.x + g.w, g.tabs_y + g.tabs_h), line_br.Get(), 1.0f);

    if (input.tab == NavPanelTab::Favorites) {
        // Album view-mode toggle button (tree ⇄ folder icons), P3.
        const D2D1_RECT_F tgl = D2D1::RectF(
            g.toggle_x, g.toggle_y, g.toggle_x + g.toggle_w,
            g.toggle_y + g.toggle_h);
        m_d2d_context->FillRoundedRectangle(
            D2D1::RoundedRect(tgl, 4.0f * dpi_s, 4.0f * dpi_s),
            hover_bg.Get());
        m_d2d_context->DrawRoundedRectangle(
            D2D1::RoundedRect(tgl, 4.0f * dpi_s, 4.0f * dpi_s),
            line_br.Get(), 1.0f);
        const std::wstring view_label = input.icons_mode == 3
            ? L"3×3"
            : input.icons_mode == 2 ? L"2×2" : L"树形";
        const float vw = measure_text(view_label, fs * dpi_s);
        const float vh = label_height(view_label, vw + 4.0f, fs, 1);
        draw_text_line(g.toggle_x + (g.toggle_w - vw) * 0.5f,
            g.toggle_y + (g.toggle_h - vh) * 0.5f, vw + 4.0f,
            view_label, text_br.Get(), fs, nullptr, 1);

        if (input.album_rows) {
            const float row_h = layout::kNavRowHeightDip * dpi_s;
            const float indent = layout::kNavIndentDip * dpi_s;
            const float row_left = g.tree_x + pad;
            const float row_right = g.tree_x + g.tree_w - pad
                - g.scrollbar_w - dt::kSpaceXsDip * dpi_s;
            const D2D1_RECT_F clip = D2D1::RectF(
                g.tree_x, g.tree_y, g.tree_x + g.tree_w,
                g.tree_y + g.tree_h);
            m_d2d_context->PushAxisAlignedClip(&clip,
                D2D1_ANTIALIAS_MODE_ALIASED);
            int list_count = static_cast<int>(input.album_rows->size());
            if (input.icons_mode > 0) {
                list_count = 0;
                while (list_count
                        < static_cast<int>(input.album_rows->size())
                    && (*input.album_rows)[static_cast<size_t>(list_count)].kind
                        != AlbumPanelRow::Kind::Folder)
                    ++list_count;
            }
            for (int i = 0; i < list_count; ++i) {
                const auto& row =
                    (*input.album_rows)[static_cast<size_t>(i)];
                const float y = g.tree_y + pad
                    + static_cast<float>(i) * row_h;
                if (y >= g.tree_y + g.tree_h) break;
                if (row.selected) {
                    m_d2d_context->FillRectangle(
                        D2D1::RectF(g.tree_x, y, g.tree_x + g.tree_w,
                            y + row_h),
                        sel_bg.Get());
                } else if (i == input.album_row_hover) {
                    m_d2d_context->FillRectangle(
                        D2D1::RectF(g.tree_x, y, g.tree_x + g.tree_w,
                            y + row_h),
                        hover_bg.Get());
                }
                const float name_x = row_left
                    + indent * static_cast<float>(row.depth);
                const float avail = std::max(0.0f, row_right - name_x);
                ID2D1SolidColorBrush* name_br = row.error
                    ? error_br.Get()
                    : (row.kind == AlbumPanelRow::Kind::Favourites
                        ? accent_br.Get() : text_br.Get());
                float badge_w = 0.0f;
                float count_w = 0.0f;
                std::wstring count_text;
                if (row.recursive) {
                    badge_w = measure_text(
                        L"[递归]", small_fs * dpi_s);
                }
                if (!row.error && row.image_count >= 0) {
                    count_text =
                        L"(" + std::to_wstring(row.image_count) + L")";
                    count_w =
                        measure_text(count_text, small_fs * dpi_s);
                }
                const float reserved = badge_w + count_w
                    + ((badge_w > 0.0f || count_w > 0.0f)
                        ? dt::kSpace6Dip * dpi_s : 0.0f);
                const float name_w = std::max(8.0f, avail - reserved);
                const float th = label_height(row.name, name_w, fs, 1);
                const float ty = y + (row_h - th) * 0.5f;
                draw_text_line(name_x, ty, name_w, row.name, name_br,
                    fs, nullptr, 1);
                if (badge_w > 0.0f) {
                    draw_text_line(name_x + name_w + dt::kSpace6Dip * dpi_s, ty,
                        badge_w + 4.0f, L"[递归]",
                        badge_br.Get(), small_fs, nullptr, 1);
                }
                if (count_w > 0.0f) {
                    draw_text_line(row_right - count_w, ty,
                        count_w + 4.0f, count_text, dim_br.Get(),
                        small_fs, nullptr, 1);
                }
            }
            if (input.icons_mode > 0) {
                const auto cells = build_folder_icon_layout(
                    *input.album_rows, g, input.icons_mode, dpi_s);
                for (const auto& cell : cells) {
                    if (cell.y >= g.tree_y + g.tree_h) break;
                    const auto& row = (*input.album_rows)
                        [static_cast<size_t>(cell.row_index)];
                    const D2D1_RECT_F rect = D2D1::RectF(
                        cell.x, cell.y, cell.x + cell.w, cell.y + cell.h);
                    m_d2d_context->FillRoundedRectangle(
                        D2D1::RoundedRect(rect, 4.0f * dpi_s,
                            4.0f * dpi_s),
                        hover_bg.Get());
                    ID2D1SolidColorBrush* border = line_br.Get();
                    if (row.error) border = error_br.Get();
                    else if (cell.row_index == input.album_row_hover)
                        border = accent_br.Get();
                    m_d2d_context->DrawRoundedRectangle(
                        D2D1::RoundedRect(rect, 4.0f * dpi_s,
                            4.0f * dpi_s),
                        border, row.error ? 2.0f : 1.0f);
                    if (row.error) {
                        const std::wstring tag = L"路径无效";
                        const float tw =
                            measure_text(tag, small_fs * dpi_s);
                        const float th2 =
                            label_height(tag, tw + 4.0f, small_fs, 1);
                        draw_text_line(cell.x + (cell.w - tw) * 0.5f,
                            cell.y + (cell.h - th2) * 0.5f, tw + 4.0f,
                            tag, error_br.Get(), small_fs, nullptr, 1);
                    } else if (input.folder_tiles
                        && cell.row_index
                            < static_cast<int>(input.folder_tiles->size())) {
                        const auto& tiles = (*input.folder_tiles)
                            [static_cast<size_t>(cell.row_index)].tiles;
                        if (!tiles.empty()) {
                            const float half_w = cell.w * 0.5f;
                            const float half_h = cell.h * 0.5f;
                            for (size_t t = 0;
                                 t < tiles.size() && t < 4; ++t) {
                                if (!tiles[t]) continue;
                                const D2D1_SIZE_F src = tiles[t]->GetSize();
                                if (src.width <= 0.0f || src.height <= 0.0f)
                                    continue;
                                const float sx = static_cast<float>(t % 2)
                                    * half_w;
                                const float sy = static_cast<float>(t / 2)
                                    * half_h;
                                const D2D1_RECT_F dest = D2D1::RectF(
                                    cell.x + sx, cell.y + sy,
                                    cell.x + sx + half_w,
                                    cell.y + sy + half_h);
                                // center-crop into the quadrant
                                const float scale = std::max(
                                    half_w / src.width,
                                    half_h / src.height);
                                const float cw = half_w / scale;
                                const float ch = half_h / scale;
                                const D2D1_RECT_F src_rect = D2D1::RectF(
                                    (src.width - cw) * 0.5f,
                                    (src.height - ch) * 0.5f,
                                    (src.width + cw) * 0.5f,
                                    (src.height + ch) * 0.5f);
                                m_d2d_context->DrawBitmap(tiles[t].Get(),
                                    dest, 1.0f,
                                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                                    src_rect);
                            }
                        }
                    }
                    std::wstring name = row.name;
                    const size_t sep = name.find_last_of(L"\\/");
                    if (sep != std::wstring::npos)
                        name = name.substr(sep + 1);
                    if (row.recursive) name += L" *";
                    const float nw = std::min(cell.w + dt::kSpaceSmDip * dpi_s,
                        measure_text(name, small_fs * dpi_s) + 4.0f);
                    draw_text_line(cell.x + (cell.w - nw) * 0.5f,
                        cell.label_y, nw, name,
                        row.error ? error_br.Get() : dim_br.Get(),
                        small_fs, nullptr, 1);
                }
            }
            m_d2d_context->PopAxisAlignedClip();
        }
    } else if (input.rows) {
        const float indent = layout::kNavIndentDip * dpi_s;
        const float arrow_w = layout::kNavArrowWidthDip * dpi_s;
        const float row_left = g.tree_x + pad;
        const float row_right = g.tree_x + g.tree_w - pad
            - g.scrollbar_w - dt::kSpaceXsDip * dpi_s;
        const D2D1_RECT_F clip =
            D2D1::RectF(g.tree_x, g.tree_y, g.tree_x + g.tree_w,
                g.tree_y + g.tree_h);
        m_d2d_context->PushAxisAlignedClip(&clip, D2D1_ANTIALIAS_MODE_ALIASED);
        for (int i = 0; i < static_cast<int>(input.rows->size()); ++i) {
            const auto& row = (*input.rows)[static_cast<size_t>(i)];
            const float y = row.y + g.tree_y - input.tree_scroll;
            if (row.highlighted) {
                m_d2d_context->FillRectangle(
                    D2D1::RectF(g.tree_x, y, g.tree_x + g.tree_w,
                        y + row.height),
                    sel_bg.Get());
            } else if (i == input.row_hover) {
                m_d2d_context->FillRectangle(
                    D2D1::RectF(g.tree_x, y, g.tree_x + g.tree_w,
                        y + row.height),
                    hover_bg.Get());
            }

            const float arrow_x =
                row_left + indent * static_cast<float>(row.depth);
            if (row.expandable) {
                const std::wstring glyph = row.loading
                    ? L"\u2026"
                    : (row.expanded ? L"\u25BE" : L"\u25B6");
                ID2D1SolidColorBrush* glyph_br =
                    row.loading ? dim_br.Get() : text_br.Get();
                const float gw = measure_text(glyph, fs * dpi_s);
                const float gh = label_height(glyph, gw + 4.0f, fs, 1);
                draw_text_line(arrow_x + (arrow_w - gw) * 0.5f,
                    y + (row.height - gh) * 0.5f, gw + 4.0f, glyph,
                    glyph_br, fs, nullptr, 1);
            }

            const float name_x = arrow_x + arrow_w;
            const float avail = std::max(0.0f, row_right - name_x);
            const std::wstring display =
                row.error_text.empty() ? row.name : row.error_text;
            ID2D1SolidColorBrush* name_br =
                row.error ? error_br.Get() : text_br.Get();
            float badge_w = 0.0f;
            float count_w = 0.0f;
            std::wstring count_text;
            if (!row.error && row.highlighted && input.highlight_recursive) {
                badge_w = measure_text(L"[\u9012\u5F52]", small_fs * dpi_s);
            }
            if (!row.error && row.image_count >= 0) {
                count_text = L"(" + std::to_wstring(row.image_count) + L")";
                count_w = measure_text(count_text, small_fs * dpi_s);
            }
            const float reserved = badge_w + count_w
                + ((badge_w > 0.0f || count_w > 0.0f) ? dt::kSpace6Dip * dpi_s : 0.0f);
            const float name_w = std::max(8.0f, avail - reserved);
            const float th = label_height(display, name_w, fs, 1);
            const float ty = y + (row.height - th) * 0.5f;
            draw_text_line(name_x, ty, name_w, display, name_br, fs,
                nullptr, 1);
            if (badge_w > 0.0f) {
                draw_text_line(name_x + name_w + dt::kSpace6Dip * dpi_s, ty,
                    badge_w + 4.0f, L"[\u9012\u5F52]", badge_br.Get(),
                    small_fs, nullptr, 1);
            }
            if (count_w > 0.0f) {
                draw_text_line(row_right - count_w, ty, count_w + 4.0f,
                    count_text, dim_br.Get(), small_fs, nullptr, 1);
            }
        }
        m_d2d_context->PopAxisAlignedClip();

        // Tree scrollbar (passive thumb; wheel-driven, drag is P4)
        if (input.tree_total > 0.0f && g.tree_h > 0.0f) {
            draw_scrollbar(g.scrollbar_x, g.tree_y, g.scrollbar_w, g.tree_h,
                input.tree_total, g.tree_h, input.tree_scroll,
                input.tree_scroll_active);
        }
    }

    // Bottom stats row
    m_d2d_context->DrawLine(
        D2D1::Point2F(g.x, g.stats_y),
        D2D1::Point2F(g.x + g.w, g.stats_y), line_br.Get(), 1.0f);
    if (input.stats_text && !input.stats_text->empty()) {
        const float sw = measure_text(*input.stats_text, small_fs * dpi_s);
        const float sh = label_height(*input.stats_text, sw + 4.0f,
            small_fs, 1);
        draw_text_line(g.x + pad, g.stats_y + (g.stats_h - sh) * 0.5f,
            sw + 4.0f, *input.stats_text, dim_br.Get(), small_fs, nullptr, 1);
    }
}

} // namespace mv
