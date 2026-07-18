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

} // namespace mv
