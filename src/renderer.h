#pragma once
#include <cstdint>
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

class Renderer {
public:
    Renderer();
    ~Renderer();

    bool init(HWND hwnd);
    void resize(uint32_t width, uint32_t height);
    void upload_image(IWICBitmapSource* wic_bitmap);

    bool begin_frame();
    bool end_frame();

    void draw_image();
    void draw_overlay();
    void draw_hint(const std::wstring& text);
    void draw_info_card(const std::vector<std::pair<std::wstring, std::wstring>>& items);
    void draw_side_panel(float x, float w, float h,
        ID2D1Bitmap1* preview, uint32_t pw, uint32_t ph,
        const std::vector<std::pair<std::wstring, std::wstring>>& info,
        const std::vector<std::pair<std::wstring, std::wstring>>& gen_info);

    // Create a D2D bitmap from a WIC source (for grid thumbnails)
    HRESULT create_bitmap_from_wic(IWICBitmapSource* wic, ID2D1Bitmap1** out);

    // Grid mode drawing
    void draw_grid_placeholder(float x, float y, float size, const std::wstring& name, bool selected);
    void draw_grid_thumbnail(float x, float y, float size, ID2D1Bitmap1* thumb, bool square = false);

    void clear(float r = 0.102f, float g = 0.102f, float b = 0.102f);

    void set_dpi(float dpi_x, float dpi_y);
    D2D1_SIZE_U target_size() const { return m_target_size; }
    void image_size(uint32_t& w, uint32_t& h) const {
        w = m_img_width; h = m_img_height;
    }

    void  set_scale(float s);
    void  set_offset(float x, float y);
    void  set_scroll_y(float y);
    float scale()    const { return m_scale; }
    float fit_scale() const { return m_fit_scale; }
    float offset_x() const { return m_offset_x; }
    float offset_y() const { return m_offset_y; }
    float scroll_y() const { return m_scroll_y; }

    bool is_valid() const { return m_d2d_context != nullptr; }

private:
    bool create_device_resources();
    bool create_text_resources();
    void discard_device_resources();

    HWND m_hwnd = nullptr;

    ComPtr<ID3D11Device>           m_d3d_device;
    ComPtr<ID3D11DeviceContext>    m_d3d_context;
    ComPtr<IDXGISwapChain1>        m_swap_chain;
    ComPtr<ID2D1Factory6>          m_d2d_factory;
    ComPtr<ID2D1Device5>           m_d2d_device;
    ComPtr<ID2D1DeviceContext5>    m_d2d_context;
    ComPtr<ID2D1Bitmap1>           m_image_bitmap;

    ComPtr<IDWriteFactory>         m_dwrite_factory;
    ComPtr<IDWriteTextFormat>      m_text_format;
    ComPtr<ID2D1SolidColorBrush>   m_overlay_brush;

    D2D1_SIZE_U m_target_size = {0, 0};
    uint32_t    m_img_width = 0;
    uint32_t    m_img_height = 0;
    float       m_scale = 1.0f;
    float       m_fit_scale = 1.0f;
    float       m_offset_x = 0.0f;
    float       m_offset_y = 0.0f;
    float       m_scroll_y = 0.0f;
    float       m_dpi_x = 96.0f;
    float       m_dpi_y = 96.0f;
};

} // namespace mv
