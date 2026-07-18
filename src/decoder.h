#pragma once
#include <wincodec.h>
#include <wrl/client.h>
#include <string>
#include <optional>

namespace mv {

using Microsoft::WRL::ComPtr;

struct ImageInfo {
    std::wstring path;
    uint32_t width = 0;
    uint32_t height = 0;
    double dpi_x = 96.0;
    double dpi_y = 96.0;
    GUID pixel_format = GUID_WICPixelFormat32bppPBGRA;
};

/// WIC-based image decoder — zero external dependencies.
/// Loads PNG, JPEG, BMP, GIF, TIFF, WebP*, HEIC*, AVIF*
/// (* requires Windows codec packs)
class Decoder {
public:
    Decoder();
    ~Decoder() = default;

    /// Decode image to a WIC bitmap at full resolution.
    /// Returns the bitmap source ready for Direct2D conversion.
    ComPtr<IWICBitmapSource> decode(const std::wstring& path);

    /// Decode and scale to fit within max_size (max dimension).
    ComPtr<IWICBitmapSource> decode_scaled(const std::wstring& path, uint32_t max_size);

    /// Get image metadata (size, DPI) without full decode.
    std::optional<ImageInfo> probe(const std::wstring& path);

private:
    ComPtr<IWICImagingFactory> m_factory;
    ComPtr<IWICBitmapDecoder>  create_decoder(const std::wstring& path);
    ComPtr<IWICBitmapSource>   convert_to_pbgra(IWICBitmapSource* src);
};

} // namespace mv
