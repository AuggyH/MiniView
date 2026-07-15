#pragma once
#include <d2d1_3.h>
#include <dwrite.h>
#include <wincodec.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <memory>
#include <optional>

namespace mv {

using Microsoft::WRL::ComPtr;

struct RenderTarget {
    ComPtr<ID2D1DeviceContext> ctx;
    ComPtr<ID2D1Bitmap1>       bitmap;   // current image as GPU texture
    uint32_t                   img_width = 0;
    uint32_t                   img_height = 0;
    float                      scale = 1.0f;
    float                      offset_x = 0.0f;
    float                      offset_y = 0.0f;
};

/// Direct2D renderer — GPU-accelerated image display.
/// Manages D3D device, swap chain, and D2D device context.
class Renderer {
public:
    Renderer();
    ~Renderer();

    /// Initialize with window handle. Must call before any rendering.
    bool init(HWND hwnd);

    /// Resize render target to window size.
    void resize(uint32_t width, uint32_t height);

    /// Upload a WIC bitmap to the GPU as a D2D texture.
    /// This is a FULL resolution upload — no downscaling.
    void upload_image(IWICBitmapSource* wic_bitmap);

    /// Begin a frame. Returns true if ready to draw.
    bool begin_frame();

    /// End the frame (present to screen).
    bool end_frame();

    /// Draw the current image centered in the window.
    /// Scales with GPU high-quality cubic interpolation.
    void draw_image();

    /// Clear to background color.
    void clear(float r = 0.102f, float g = 0.102f, float b = 0.102f);

    /// Get render target size.
    D2D1_SIZE_U target_size() const { return m_target_size; }

    /// Get image dimensions (0 if no image loaded).
    void image_size(uint32_t& w, uint32_t& h) const {
        w = m_img_width; h = m_img_height;
    }

    /// Adjust zoom (scale factor, 1.0 = fit-to-window, >1 = zoom in).
    void set_scale(float s);
    void set_offset(float x, float y);
    float scale() const { return m_scale; }

private:
    bool create_device_resources(HWND hwnd);
    void discard_device_resources();

    ComPtr<ID3D11Device>           m_d3d_device;
    ComPtr<ID3D11DeviceContext>    m_d3d_context;
    ComPtr<IDXGISwapChain1>        m_swap_chain;
    ComPtr<ID2D1Factory6>          m_d2d_factory;
    ComPtr<ID2D1Device5>           m_d2d_device;
    ComPtr<ID2D1DeviceContext5>    m_d2d_context;
    ComPtr<ID2D1Bitmap1>           m_image_bitmap;  // GPU texture of current image

    D2D1_SIZE_U m_target_size = {0, 0};
    uint32_t    m_img_width = 0;
    uint32_t    m_img_height = 0;
    float       m_scale = 1.0f;
    float       m_offset_x = 0.0f;
    float       m_offset_y = 0.0f;
};

} // namespace mv
