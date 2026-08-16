#pragma once
// 快速读取常见图片格式的真实宽高: 只解析文件头, 不创建 WIC 解码器。
// 用于网格首帧布局——恢复"骨架屏按真实比例布局、无漂移", 同时避免
// 逐文件 WIC probe 的秒级卡顿。不支持的格式返回 nullopt, 由调用方回退。
#include <cstdint>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace mv {

inline uint16_t read_be16(const unsigned char* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
inline uint32_t read_be32(const unsigned char* p) {
    return (static_cast<uint32_t>(p[0]) << 24)
        | (static_cast<uint32_t>(p[1]) << 16)
        | (static_cast<uint32_t>(p[2]) << 8)
        | static_cast<uint32_t>(p[3]);
}
inline uint32_t read_le32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0])
        | (static_cast<uint32_t>(p[1]) << 8)
        | (static_cast<uint32_t>(p[2]) << 16)
        | (static_cast<uint32_t>(p[3]) << 24);
}

inline std::optional<std::pair<uint32_t, uint32_t>> fast_image_dimensions(
    const std::wstring& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return std::nullopt;
    unsigned char hdr[64] = {};
    f.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
    const std::streamsize got = f.gcount();
    if (got < 24) return std::nullopt;

    // PNG: 8-byte signature + "IHDR" chunk, width/height at bytes 16/20.
    if (hdr[0] == 0x89 && hdr[1] == 'P' && hdr[2] == 'N' && hdr[3] == 'G'
        && hdr[12] == 'I' && hdr[13] == 'H' && hdr[14] == 'D' && hdr[15] == 'R') {
        return std::make_pair(read_be32(hdr + 16), read_be32(hdr + 20));
    }

    // JPEG: walk markers for an SOF segment (C0-CF, skip C4/C8/CC).
    if (hdr[0] == 0xFF && hdr[1] == 0xD8 && got >= 64) {
        size_t pos = 2;
        while (pos + 9 < 64) {
            if (hdr[pos] != 0xFF) break;
            while (pos < 64 && hdr[pos] == 0xFF) ++pos;
            const unsigned char marker = hdr[pos++];
            if (marker == 0xD9 || marker == 0xDA) break;  // EOI / SOS
            if (marker >= 0xC0 && marker <= 0xCF
                && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
                if (pos + 7 >= 64) break;
                const uint16_t height = read_be16(hdr + pos + 3);
                const uint16_t width = read_be16(hdr + pos + 5);
                if (width > 0 && height > 0)
                    return std::make_pair(width, height);
                break;
            }
            if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD8)) continue;
            if (pos + 2 > 64) break;
            const uint16_t len = read_be16(hdr + pos);
            if (len < 2) break;
            pos += len;
        }
        return std::nullopt;  // 头部 64 字节内没有 SOF, 交给 WIC 回退
    }

    // GIF: "GIF87a"/"GIF89a", little-endian width/height at 6/8.
    if (hdr[0] == 'G' && hdr[1] == 'I' && hdr[2] == 'F') {
        const uint32_t w = static_cast<uint32_t>(hdr[6])
            | (static_cast<uint32_t>(hdr[7]) << 8);
        const uint32_t h = static_cast<uint32_t>(hdr[8])
            | (static_cast<uint32_t>(hdr[9]) << 8);
        return std::make_pair(w, h);
    }

    // BMP: "BM", width/height (signed, take abs) at 18/22.
    if (hdr[0] == 'B' && hdr[1] == 'M') {
        const int32_t w = static_cast<int32_t>(read_le32(hdr + 18));
        const int32_t h = static_cast<int32_t>(read_le32(hdr + 22));
        if (w > 0 && h != 0)
            return std::make_pair(static_cast<uint32_t>(w),
                static_cast<uint32_t>(h < 0 ? -h : h));
    }

    // WebP: RIFF....WEBP, then VP8 /VP8L/VP8X chunk.
    if (hdr[0] == 'R' && hdr[1] == 'I' && hdr[2] == 'F' && hdr[3] == 'F'
        && hdr[8] == 'W' && hdr[9] == 'E' && hdr[10] == 'B' && hdr[11] == 'P') {
        if (hdr[12] == 'V' && hdr[13] == 'P' && hdr[14] == '8' && hdr[15] == ' ') {
            if (got >= 30 && hdr[20] == 0x9D && hdr[21] == 0x01 && hdr[22] == 0x2A) {
                const uint32_t w = read_le32(hdr + 26) & 0x3FFFu;
                const uint32_t h = read_le32(hdr + 28) & 0x3FFFu;
                if (w > 0 && h > 0) return std::make_pair(w, h);
            }
        } else if (hdr[12] == 'V' && hdr[13] == 'P' && hdr[14] == '8' && hdr[15] == 'L') {
            if (got >= 25 && hdr[20] == 0x2F) {
                uint32_t bits = read_le32(hdr + 21);
                const uint32_t w = (bits & 0x3FFF) + 1;
                const uint32_t h = ((bits >> 14) & 0x3FFF) + 1;
                return std::make_pair(w, h);
            }
        } else if (hdr[12] == 'V' && hdr[13] == 'P' && hdr[14] == '8' && hdr[15] == 'X') {
            if (got >= 30) {
                const uint32_t w = (static_cast<uint32_t>(hdr[24])
                    | (static_cast<uint32_t>(hdr[25]) << 8)
                    | (static_cast<uint32_t>(hdr[26]) << 16)) + 1;
                const uint32_t h = (static_cast<uint32_t>(hdr[27])
                    | (static_cast<uint32_t>(hdr[28]) << 8)
                    | (static_cast<uint32_t>(hdr[29]) << 16)) + 1;
                return std::make_pair(w, h);
            }
        }
    }

    return std::nullopt;
}

} // namespace mv
