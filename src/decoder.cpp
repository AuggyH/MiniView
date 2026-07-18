#include "decoder.h"
#include <stdexcept>

namespace mv {

Decoder::Decoder() {
    HRESULT hr = CoCreateInstance(
        CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&m_factory));
    if (FAILED(hr))
        throw std::runtime_error("Failed to create WIC factory");
}

ComPtr<IWICBitmapDecoder> Decoder::create_decoder(const std::wstring& path) {
    ComPtr<IWICBitmapDecoder> decoder;
    HRESULT hr = m_factory->CreateDecoderFromFilename(
        path.c_str(), nullptr, GENERIC_READ,
        WICDecodeMetadataCacheOnDemand,
        &decoder);
    if (FAILED(hr))
        throw std::runtime_error("Failed to create decoder for image");
    return decoder;
}

ComPtr<IWICBitmapSource> Decoder::convert_to_pbgra(IWICBitmapSource* src) {
    ComPtr<IWICFormatConverter> converter;
    HRESULT hr = m_factory->CreateFormatConverter(&converter);
    if (FAILED(hr))
        throw std::runtime_error("Failed to create format converter");

    // Convert to 32-bit PBGRA (Direct2D's native format) with full fidelity
    hr = converter->Initialize(
        src,
        GUID_WICPixelFormat32bppPBGRA,
        WICBitmapDitherTypeNone,
        nullptr, 0.0,
        WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr))
        throw std::runtime_error("Failed to convert to PBGRA");

    ComPtr<IWICBitmapSource> result;
    converter.As(&result);
    return result;
}

ComPtr<IWICBitmapSource> Decoder::decode(const std::wstring& path) {
    auto decoder = create_decoder(path);

    // Get the first frame (for multi-frame images like GIF, we take frame 0)
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
        throw std::runtime_error("Failed to get image frame");

    // Convert to GPU-friendly format at FULL resolution — no downscaling
    return convert_to_pbgra(frame.Get());
}

ComPtr<IWICBitmapSource> Decoder::decode_scaled(const std::wstring& path, uint32_t max_size) {
    auto decoder = create_decoder(path);

    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr))
        throw std::runtime_error("Failed to get image frame");

    // Scale down if needed
    uint32_t fw, fh;
    frame->GetSize(&fw, &fh);
    if (fw > max_size || fh > max_size) {
        float scale = static_cast<float>(max_size) / std::max(fw, fh);
        uint32_t sw = static_cast<uint32_t>(fw * scale);
        uint32_t sh = static_cast<uint32_t>(fh * scale);

        ComPtr<IWICBitmapScaler> scaler;
        hr = m_factory->CreateBitmapScaler(&scaler);
        if (FAILED(hr))
            throw std::runtime_error("Failed to create bitmap scaler");

        // Two-step downscale for large ratios: fast Linear to 2x, then Fant to final
        float ratio = std::max(fw, fh) / static_cast<float>(max_size);
        if (ratio > 3.0f) {
            uint32_t mid_w = static_cast<uint32_t>(fw * scale * 2.0f);
            uint32_t mid_h = static_cast<uint32_t>(fh * scale * 2.0f);
            if (mid_w > max_size * 2) mid_w = max_size * 2;
            if (mid_h > max_size * 2) mid_h = max_size * 2;
            hr = scaler->Initialize(frame.Get(), mid_w, mid_h, WICBitmapInterpolationModeLinear);
            if (FAILED(hr))
                throw std::runtime_error("Failed to scale bitmap (step 1)");

            ComPtr<IWICBitmapScaler> scaler2;
            hr = m_factory->CreateBitmapScaler(&scaler2);
            if (FAILED(hr))
                throw std::runtime_error("Failed to create bitmap scaler 2");
            hr = scaler2->Initialize(scaler.Get(), sw, sh, WICBitmapInterpolationModeFant);
            if (FAILED(hr))
                throw std::runtime_error("Failed to scale bitmap (step 2)");
            return convert_to_pbgra(scaler2.Get());
        }

        hr = scaler->Initialize(frame.Get(), sw, sh, WICBitmapInterpolationModeFant);
        if (FAILED(hr))
            throw std::runtime_error("Failed to scale bitmap");

        return convert_to_pbgra(scaler.Get());
    }

    return convert_to_pbgra(frame.Get());
}

std::optional<ImageInfo> Decoder::probe(const std::wstring& path) {
    try {
        auto decoder = create_decoder(path);
        ComPtr<IWICBitmapFrameDecode> frame;
        HRESULT hr = decoder->GetFrame(0, &frame);
        if (FAILED(hr)) return std::nullopt;

        ImageInfo info;
        info.path = path;
        frame->GetSize(&info.width, &info.height);

        // Get DPI for proper scaling
        double dpi_x = 96.0, dpi_y = 96.0;
        frame->GetResolution(&dpi_x, &dpi_y);
        if (dpi_x > 0) info.dpi_x = dpi_x;
        if (dpi_y > 0) info.dpi_y = dpi_y;

        // Get pixel format
        frame->GetPixelFormat(&info.pixel_format);

        return info;
    } catch (...) {
        return std::nullopt;
    }
}

D2D1_COLOR_F Decoder::extract_dominant(IWICBitmapSource* src) {
    D2D1_COLOR_F fallback = {0.10f, 0.10f, 0.12f, 1.0f};
    if (!src || !m_factory) return fallback;

    try {
        // Downscale to 32x32 for fast histogram
        ComPtr<IWICBitmapScaler> scaler;
        HRESULT hr = m_factory->CreateBitmapScaler(&scaler);
        if (FAILED(hr)) return fallback;

        uint32_t sw, sh;
        src->GetSize(&sw, &sh);
        float scl = 32.0f / std::max(sw, sh);
        uint32_t dw = std::max(1u, static_cast<uint32_t>(sw * scl));
        uint32_t dh = std::max(1u, static_cast<uint32_t>(sh * scl));
        hr = scaler->Initialize(src, dw, dh, WICBitmapInterpolationModeFant);
        if (FAILED(hr)) return fallback;

        // Convert to 32bpp RGBA
        ComPtr<IWICFormatConverter> converter;
        hr = m_factory->CreateFormatConverter(&converter);
        if (FAILED(hr)) return fallback;
        hr = converter->Initialize(scaler.Get(), GUID_WICPixelFormat32bppRGBA,
            WICBitmapDitherTypeNone, nullptr, 0.0f, WICBitmapPaletteTypeCustom);
        if (FAILED(hr)) return fallback;

        // Copy to lockable bitmap
        ComPtr<IWICBitmap> bmp;
        hr = m_factory->CreateBitmapFromSource(converter.Get(), WICBitmapCacheOnDemand, &bmp);
        if (FAILED(hr)) return fallback;

        // Lock and read pixels
        WICRect rc = {0, 0, static_cast<INT>(dw), static_cast<INT>(dh)};
        ComPtr<IWICBitmapLock> lock;
        hr = bmp->Lock(&rc, WICBitmapLockRead, &lock);
        if (FAILED(hr)) return fallback;

        UINT stride = 0, size = 0;
        BYTE* data = nullptr;
        lock->GetDataPointer(&size, &data);
        lock->GetStride(&stride);
        if (!data || size == 0) return fallback;

        // Histogram: quantize each channel to 4 bits (16 levels) → 4096 buckets
        int buckets[4096] = {};
        for (UINT y = 0; y < dh; ++y) {
            for (UINT x = 0; x < dw; ++x) {
                BYTE* p = data + y * stride + x * 4;
                int r = p[0] >> 4;  // 0–15
                int g = p[1] >> 4;
                int b = p[2] >> 4;
                ++buckets[(r << 8) | (g << 4) | b];
            }
        }

        // Find max bucket
        int max_idx = 0, max_cnt = 0;
        for (int i = 0; i < 4096; ++i) {
            if (buckets[i] > max_cnt) {
                max_cnt = buckets[i];
                max_idx = i;
            }
        }

        // Convert bucket index back to color (use bucket center)
        int br = (max_idx >> 8) & 0xF;
        int bg = (max_idx >> 4) & 0xF;
        int bb = max_idx & 0xF;
        D2D1_COLOR_F c;
        c.r = (br * 16.0f + 8.0f) / 255.0f;
        c.g = (bg * 16.0f + 8.0f) / 255.0f;
        c.b = (bb * 16.0f + 8.0f) / 255.0f;
        c.a = 1.0f;
        return c;

    } catch (...) {
        return fallback;
    }
}

} // namespace mv
