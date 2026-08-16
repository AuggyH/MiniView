#include "decoder.h"
#include "fast_image_dims.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <objbase.h>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

class TempDirectory {
public:
    TempDirectory() {
        const auto suffix =
            std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::temp_directory_path()
            / (L"minview-decoder-tests-" + std::to_wstring(suffix));
        fs::create_directories(m_path);
    }
    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(m_path, error);
    }
    const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

void write_u32_le(std::ofstream& output, uint32_t value) {
    const std::array<unsigned char, 4> bytes = {
        static_cast<unsigned char>(value & 0xff),
        static_cast<unsigned char>((value >> 8) & 0xff),
        static_cast<unsigned char>((value >> 16) & 0xff),
        static_cast<unsigned char>((value >> 24) & 0xff)
    };
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void write_u16_le(std::ofstream& output, uint16_t value) {
    const std::array<unsigned char, 2> bytes = {
        static_cast<unsigned char>(value & 0xff),
        static_cast<unsigned char>((value >> 8) & 0xff)
    };
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

// Minimal 24-bit bottom-up BMP that WIC can decode without extension packs.
void write_bmp(const fs::path& path, uint32_t width, uint32_t height) {
    const uint32_t row_bytes = (width * 3 + 3) & ~3u;
    const uint32_t image_bytes = row_bytes * height;
    std::ofstream output(path, std::ios::binary);
    output.put('B');
    output.put('M');
    write_u32_le(output, 54 + image_bytes);
    write_u32_le(output, 0);
    write_u32_le(output, 54);
    write_u32_le(output, 40);
    write_u32_le(output, width);
    write_u32_le(output, height);
    write_u16_le(output, 1);
    write_u16_le(output, 24);
    write_u32_le(output, 0);
    write_u32_le(output, image_bytes);
    write_u32_le(output, 2835);
    write_u32_le(output, 2835);
    write_u32_le(output, 0);
    write_u32_le(output, 0);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            output.put(static_cast<char>(255));  // B
            output.put(static_cast<char>(0));    // G
            output.put(static_cast<char>(0));    // R
        }
        for (uint32_t pad = width * 3; pad < row_bytes; ++pad)
            output.put('\0');
    }
}

std::wstring corrupt_fixture_path() {
    return std::wstring(MINVIEW_SOURCE_DIR) + L"\\tests\\corrupt_open_input.png";
}

struct ScopedCom {
    HRESULT m_result;
    ScopedCom() : m_result(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {}
    ~ScopedCom() {
        if (SUCCEEDED(m_result)) CoUninitialize();
    }
};

} // namespace

int main() {
    ScopedCom com;
    TempDirectory temp;
    try {
        mv::Decoder decoder;

    // Missing files must throw with the path and HRESULT in the message,
    // and probe must fail closed instead of throwing.
    const fs::path missing = temp.path() / L"missing.png";
    bool threw = false;
    try {
        (void)decoder.decode(missing.wstring());
    } catch (const std::exception& error) {
        threw = true;
        const std::string message = error.what();
        expect(message.find("missing.png") != std::string::npos,
            "decode error must include the failing path");
        expect(message.find("HRESULT=") != std::string::npos,
            "decode error must include the HRESULT");
    }
    expect(threw, "decode of a missing file must throw");
    expect(!decoder.probe(missing.wstring()).has_value(),
        "probe of a missing file must return nullopt");

    // Corrupted input must throw with the path and HRESULT.
    const fs::path corrupt = corrupt_fixture_path();
    bool corrupt_threw = false;
    try {
        (void)decoder.decode(corrupt.wstring());
    } catch (const std::exception& error) {
        corrupt_threw = true;
        expect(std::string(error.what()).find("HRESULT=") != std::string::npos,
            "corrupt-image error must include the HRESULT");
    }
    expect(corrupt_threw, "decode of the corrupt fixture must throw");

    // Valid BMP round-trip: full decode, scaled decode and probe agree.
    const fs::path valid = temp.path() / L"valid.bmp";
    write_bmp(valid, 8, 6);
    auto full = decoder.decode(valid.wstring());
    uint32_t width = 0, height = 0;
    if (full) full->GetSize(&width, &height);
    expect(full && width == 8 && height == 6, "valid BMP must decode at full size");

    auto scaled = decoder.decode_scaled(valid.wstring(), 4);
    uint32_t scaled_w = 0, scaled_h = 0;
    if (scaled) scaled->GetSize(&scaled_w, &scaled_h);
    expect(scaled && scaled_w == 4 && scaled_h == 3,
        "decode_scaled must fit the max dimension and preserve aspect ratio");

    auto info = decoder.probe(valid.wstring());
    expect(info && info->width == 8 && info->height == 6,
        "probe must report the BMP dimensions without full decode");

    // Null materialize is a programming error and must fail loudly.
    bool materialize_threw = false;
    try {
        (void)decoder.materialize(nullptr);
    } catch (const std::exception&) {
        materialize_threw = true;
    }
    expect(materialize_threw, "materialize(nullptr) must throw");

    // 文件头快速尺寸解析(网格无漂移首帧布局依赖它)
    {
        const fs::path png = temp.path() / L"dims.png";
        std::ofstream out(png, std::ios::binary);
        const std::array<unsigned char, 8> sig = {
            0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        out.write(reinterpret_cast<const char*>(sig.data()), sig.size());
        auto put_be = [&](uint32_t v) {
            out.put(static_cast<char>((v >> 24) & 0xff));
            out.put(static_cast<char>((v >> 16) & 0xff));
            out.put(static_cast<char>((v >> 8) & 0xff));
            out.put(static_cast<char>(v & 0xff));
        };
        put_be(13);                        // IHDR length
        out.write("IHDR", 4);
        put_be(1234);                      // width
        put_be(567);                       // height
        out.put(8); out.put(6); out.put(0); out.put(0); out.put(0);  // bit/color/comp/filter/interlace
        out.close();
        const auto png_dims = mv::fast_image_dimensions(png.wstring());
        expect(png_dims && png_dims->first == 1234 && png_dims->second == 567,
            "fast_image_dimensions must read PNG IHDR without WIC");
        const auto bmp_dims = mv::fast_image_dimensions(valid.wstring());
        expect(bmp_dims && bmp_dims->first == 8 && bmp_dims->second == 6,
            "fast_image_dimensions must read BMP header without WIC");
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "decoder_tests: PASS\n";
    return 0;
    } catch (const std::exception& error) {
        std::cerr << "uncaught exception: " << error.what() << '\n';
        return 2;
    } catch (...) {
        std::cerr << "uncaught non-standard exception\n";
        return 2;
    }
}
