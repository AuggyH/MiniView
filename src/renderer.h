#pragma once
#include "design_tokens.h"
#include "renderer_state.h"
#include "layout.h"

#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <d2d1_3.h>
#include <dwrite.h>
#include <wincodec.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace mv {

using Microsoft::WRL::ComPtr;

struct PanelRegion { D2D1_RECT_F rect; std::wstring text; std::wstring label; };

struct ComicPageDrawItem {
    ComicPageGeometry geometry;
    ID2D1Bitmap1* bitmap = nullptr;
    bool failed = false;
};

struct FilmstripRenderItem {
    int index = -1;
    float left = 0.0f;   // strip-local coordinates (physical px)
    float top = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    ID2D1Bitmap1* bitmap = nullptr;
    D2D1_COLOR_F placeholder_color = dt::d2d(dt::kColorPlaceholder);
    bool current = false;
    float zoom = 1.0f;  // magnified zoom (border alpha + float ordering)
};

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool init(HWND hwnd);
    bool resize(uint32_t width, uint32_t height);
    bool upload_image(IWICBitmapSource* wic_bitmap, bool reset_view = true);
    // Provisional thumbnail placeholder (progressive paging): drawn instead
    // of the full image while its decode is still in flight.
    void set_placeholder(ID2D1Bitmap1* bmp);
    void clear_placeholder();

    bool begin_frame();
    bool end_frame();

    void draw_image();
    void ensure_image_scaled();
    void draw_overlay(float bottom_inset = 0.0f);
    void draw_hint(const std::wstring& text);
    void draw_status_message(const std::wstring& text);
    void draw_info_card(const std::vector<std::pair<std::wstring, std::wstring>>& items);
    float draw_side_panel(float x, float y_off, float w, float h,
        ID2D1Bitmap1* preview, uint32_t pw, uint32_t ph,
        const std::vector<std::pair<std::wstring, std::wstring>>& info,
        const std::vector<std::pair<std::wstring, std::wstring>>& gen_info,
        std::vector<PanelRegion>* out_clickable = nullptr,
        int sel_idx = -1, const std::wstring* toast = nullptr,
        float scroll_y = 0.0f);  // returns total content height
    void draw_scrollbar(float x, float y, float w, float h,
        float total, float view, float pos, bool active = false);
    void draw_filmstrip(float x, float y, float w, float h,
        std::span<const FilmstripRenderItem> items,
        bool left_overflow, bool right_overflow,
        float anim_t);
    float draw_text_line(float x, float y, float w,
        const std::wstring& text, ID2D1SolidColorBrush* brush,
        float font_size = 0.0f, float* out_width = nullptr, int max_lines = 0);
    void draw_toolbar(float w, const std::vector<std::wstring>& items,
        int active_idx, float y = 0);
    void draw_title_bar(float w, int hover_btn, int press_btn,
        const std::vector<std::wstring>& menu_items, int active_menu);
    void draw_breadcrumb(const NavBreadcrumbRenderInput& input);
    void draw_nav_panel(const NavPanelRenderInput& input);
    void draw_fade_overlay(float t, bool forward);
    // Full-window opaque bitmap (the grid snapshot under the entry veil).
    void draw_fullscreen_bitmap(ID2D1Bitmap1* bmp);
    // The full image revealed inside the thumbnail\'s interpolated rect —
    // it grows with the thumbnail so the final state needs no pop.
    void draw_anim_image(ID2D1Bitmap1* image, D2D1_RECT_F src, D2D1_RECT_F dst, float t);
    void draw_anim_thumb(ID2D1Bitmap1* bmp, D2D1_RECT_F src, D2D1_RECT_F dst, float t);
    // Same zoom path with explicit layer opacity and optional exit curve.
    void draw_anim_thumb_faded(ID2D1Bitmap1* bmp, D2D1_RECT_F src,
        D2D1_RECT_F dst, float t, float alpha, bool exit_curve = false);
    void push_clip_below(float y);
    void push_clip_rect(const D2D1_RECT_F& rc);
    void pop_clip();
    float measure_text(const std::wstring& text, float font_size);

    // Create a D2D bitmap from a WIC source (for grid thumbnails)
    HRESULT create_bitmap_from_wic(IWICBitmapSource* wic, ID2D1Bitmap1** out);

    // Grid mode drawing
    void draw_grid_placeholder(float x, float y, float w, float h, D2D1_COLOR_F color);
    void draw_grid_thumbnail(float x, float y, float w, float h, ID2D1Bitmap1* thumb, bool square = false);
    // 收藏 corner badge (Issue #5 P3c): dark chip + accent heart
    void draw_favourite_badge(float x, float y, float size);
    void draw_comic_page(ID2D1Bitmap1* bitmap, D2D1_RECT_F destination);
    void draw_comic_card(D2D1_RECT_F destination, bool failed);
    void draw_comic_pages(
        std::span<const ComicPageDrawItem> pages,
        ComicRenderViewport viewport);
    void draw_comic_controls(const ComicControlsRenderInput& input);
    void draw_selection_border(D2D1_RECT_F rc, float alpha = 1.0f);
    void draw_label(float x, float y, float w, const std::wstring& text, float font_size,
        float r = dt::kColorLabelDefault.r, float g = dt::kColorLabelDefault.g,
        float b = dt::kColorLabelDefault.b);
    float label_height(const std::wstring& text, float w, float font_size, int max_lines = 2);

    void clear(float r = dt::kColorCanvas.r, float g = dt::kColorCanvas.g,
        float b = dt::kColorCanvas.b);

    void set_dpi(float dpi_x, float dpi_y);
    D2D1_SIZE_U target_size() const { return m_target_size; }
    void image_size(uint32_t& w, uint32_t& h) const {
        w = m_img_width; h = m_img_height;
    }

    void  set_scale(float s);
    void  set_offset(float x, float y);
    void set_content_viewport(float top, float right, float left = 0.0f);
    void  set_scroll_y(float y);
    float scale()    const { return m_scale; }
    float fit_scale() const { return m_fit_scale; }
    float offset_x() const { return m_offset_x; }
    float offset_y() const { return m_offset_y; }
    float scroll_y() const { return m_scroll_y; }
    float content_top() const { return m_content_top; }
    float content_left() const { return m_content_left; }
    float content_width() const;
    ID2D1Bitmap1* image_bitmap() const { return m_image_bitmap.Get(); }
    ID2D1Bitmap1* placeholder_bitmap() const { return m_placeholder_bitmap.Get(); }
    // Animation source: the pre-scaled fit cache when available, so the
    // transition never re-samples the 4K original every frame.
    ID2D1Bitmap1* transition_image() const {
        return m_image_scaled ? m_image_scaled.Get() : m_image_bitmap.Get();
    }

    // Test seam for device-loss recovery verification: release all
    // device-scoped resources exactly like a real device-loss path.
    void discard_device_resources_for_testing() {
        discard_device_resources();
    }

    bool is_valid() const { return m_d2d_context != nullptr; }
    uint64_t device_generation() const { return m_device_generation; }

private:
    bool create_device_resources();
    bool create_text_resources();
    void discard_device_resources();
    void update_fit_scale();
    void draw_comic_bitmap(
        ID2D1Bitmap1* bitmap, D2D1_RECT_F destination,
        float corner_radius);
    void draw_comic_card_visual(
        D2D1_RECT_F destination, D2D1_RECT_F text_bounds,
        ComicPageVisual visual, const ComicRenderMetrics& metrics);
    void draw_filmstrip_arrow(float cx, float cy, const wchar_t* glyph,
        float dpi_scale);

    // ── Device-scoped render resource caches ──────────────────
    // Solid brushes, text formats, and rounded-rectangle geometries were
    // created per frame/per item. They are immutable once configured (the
    // text-format helper resets the few mutable layout fields before it
    // returns), so caching them is pixel-identical while removing the
    // per-frame D2D allocation churn.
    ComPtr<ID2D1SolidColorBrush> get_solid_brush(const D2D1_COLOR_F& color);
    ComPtr<IDWriteTextFormat> get_text_format(
        const wchar_t* family, float size,
        DWRITE_FONT_WEIGHT weight, DWRITE_FONT_STYLE style,
        DWRITE_FONT_STRETCH stretch, const wchar_t* locale);
    ComPtr<ID2D1RoundedRectangleGeometry> get_rounded_geometry(
        float width, float height, float radius);

    struct SolidBrushCacheEntry {
        D2D1_COLOR_F color{};
        ComPtr<ID2D1SolidColorBrush> brush;
    };
    struct TextFormatCacheEntry {
        std::wstring family;
        float size = 0.0f;
        DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
        DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
        DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
        std::wstring locale;
        ComPtr<IDWriteTextFormat> format;
    };
    struct RoundedGeometryCacheEntry {
        float width = 0.0f;
        float height = 0.0f;
        float radius = 0.0f;
        ComPtr<ID2D1RoundedRectangleGeometry> geometry;
    };

    HWND m_hwnd = nullptr;

    std::vector<SolidBrushCacheEntry>      m_solid_brushes;
    std::vector<TextFormatCacheEntry>      m_text_formats;
    std::vector<RoundedGeometryCacheEntry> m_rounded_geometries;

    ComPtr<ID3D11Device>           m_d3d_device;
    ComPtr<ID3D11DeviceContext>    m_d3d_context;
    ComPtr<IDXGISwapChain1>        m_swap_chain;
    ComPtr<ID2D1Factory6>          m_d2d_factory;
    ComPtr<ID2D1Device5>           m_d2d_device;
    ComPtr<ID2D1DeviceContext5>    m_d2d_context;
    ComPtr<ID2D1Bitmap1>           m_image_bitmap;
    ComPtr<ID2D1Bitmap1>           m_placeholder_bitmap;  // provisional thumb
    ComPtr<ID2D1Bitmap1>           m_image_scaled;   // pre-scaled zoom cache
    float m_image_scaled_scale = -1.0f;              // scale the cache was built at

    ComPtr<IDWriteFactory>         m_dwrite_factory;
    ComPtr<IDWriteTextFormat>      m_text_format;
    ComPtr<ID2D1SolidColorBrush>   m_overlay_brush;
    ComPtr<ID2D1LinearGradientBrush> m_filmstrip_bg_gradient;  // cached (per dpi)
    float m_filmstrip_bg_dpi = 0.0f;
    float m_filmstrip_bg_width = -1.0f;
    ComPtr<ID2D1LinearGradientBrush> m_filmstrip_mask_gradient;  // edge alpha mask
    float m_filmstrip_mask_dpi = 0.0f;
    float m_filmstrip_mask_width = -1.0f;
    ComPtr<ID2D1Layer> m_filmstrip_mask_layer;  // cached, not per-frame

    D2D1_SIZE_U m_target_size = {0, 0};
    uint32_t    m_img_width = 0;
    uint32_t    m_img_height = 0;
    float       m_scale = 1.0f;
    float       m_fit_scale = 1.0f;
    float       m_offset_x = 0.0f;
    float       m_offset_y = 0.0f;
    float       m_scroll_y = 0.0f;
    float       m_content_top = 0.0f;  // title bar height, for centering below
    float       m_content_right = 0.0f;  // side panel width reserved from image viewport
    float       m_content_left = 0.0f;   // left nav panel width reserved from image viewport
    float       m_dpi_x = 96.0f;
    float       m_dpi_y = 96.0f;
    uint64_t    m_device_generation = 0;
};

} // namespace mv
