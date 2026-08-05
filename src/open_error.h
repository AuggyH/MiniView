#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>
#include <winerror.h>

namespace mv {

enum class OpenInputRoute {
    DecodeImage,
    OpenDirectory,
    MissingPath,
    UnsupportedFormat,
    ReadOrDecodeFailed,
};

enum class ImageLoadResult {
    Success,
    DecodeFailed,
    MaterializeFailed,
    UploadFailed,
};

inline bool is_supported_open_extension(std::wstring extension) {
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return extension == L".png" || extension == L".jpg"
        || extension == L".jpeg" || extension == L".jpe"
        || extension == L".jfif" || extension == L".bmp"
        || extension == L".dib" || extension == L".ico"
        || extension == L".cur"
        || extension == L".gif" || extension == L".webp"
        || extension == L".tiff" || extension == L".tif";
}

inline OpenInputRoute classify_open_input(
    bool attributes_available, bool is_directory,
    uint32_t attribute_error, const std::wstring& extension) {
    if (!attributes_available) {
        if (attribute_error == ERROR_FILE_NOT_FOUND
            || attribute_error == ERROR_PATH_NOT_FOUND
            || attribute_error == ERROR_INVALID_NAME) {
            return OpenInputRoute::MissingPath;
        }
        return OpenInputRoute::ReadOrDecodeFailed;
    }
    if (is_directory) return OpenInputRoute::OpenDirectory;
    if (!is_supported_open_extension(extension))
        return OpenInputRoute::UnsupportedFormat;
    return OpenInputRoute::DecodeImage;
}

template <typename Load>
inline OpenInputRoute resolve_open_input_route(
    OpenInputRoute route, Load load) noexcept {
    if (route != OpenInputRoute::DecodeImage) return route;
    try {
        return load() == ImageLoadResult::Success
            ? OpenInputRoute::DecodeImage : OpenInputRoute::ReadOrDecodeFailed;
    } catch (...) {
        return OpenInputRoute::ReadOrDecodeFailed;
    }
}

inline const wchar_t* open_input_error_message(OpenInputRoute route) {
    switch (route) {
    case OpenInputRoute::MissingPath:
        return L"无法打开：文件或路径不存在。";
    case OpenInputRoute::UnsupportedFormat:
        return L"无法打开：暂不支持此文件格式。";
    case OpenInputRoute::ReadOrDecodeFailed:
        return L"无法打开：文件无法读取或图片解码失败。";
    default:
        return L"";
    }
}

inline bool complete_directory_open(
    int scan_result, std::wstring& open_error) {
    if (scan_result < 0) return false;
    open_error.clear();
    return true;
}

} // namespace mv
