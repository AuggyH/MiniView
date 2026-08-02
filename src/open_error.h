#pragma once

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>
#include <winerror.h>

namespace mv {

enum class OpenInputRoute {
    LoadImage,
    OpenDirectory,
    MissingPath,
    UnsupportedFormat,
    ReadOrDecodeFailed,
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
    return OpenInputRoute::LoadImage;
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

} // namespace mv
