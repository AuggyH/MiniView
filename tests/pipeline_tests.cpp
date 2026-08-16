#include "indexer.h"
#include "decoder.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <objbase.h>
#include <stdexcept>
#include <string>
#include <vector>

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
            / (L"minview-pipeline-tests-" + std::to_wstring(suffix));
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
// Same writer pattern as decoder_tests, kept local to avoid coupling tests.
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

void write_noise_file(const fs::path& path) {
    std::ofstream output(path, std::ios::binary);
    output << "this file must be ignored by the image index\n";
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
    try {
        ScopedCom com;
        TempDirectory temp;
        mv::Decoder decoder;

        const fs::path root = temp.path();
        const fs::path level1 = root / L"level1";
        const fs::path level2 = level1 / L"level2";
        fs::create_directories(level2);

        const fs::path bmp_root = root / L"root.bmp";
        const fs::path bmp_level1 = level1 / L"mid.bmp";
        const fs::path bmp_level2 = level2 / L"leaf.bmp";
        write_bmp(bmp_root, 8, 6);
        write_bmp(bmp_level1, 16, 10);
        write_bmp(bmp_level2, 24, 18);

        const fs::path corrupt_source =
            fs::path(MINVIEW_SOURCE_DIR) / L"tests" / L"corrupt_open_input.png";
        const fs::path corrupt_copy = root / L"corrupt_open_input.png";
        std::error_code copy_error;
        fs::copy_file(corrupt_source, corrupt_copy,
            fs::copy_options::overwrite_existing, copy_error);
        expect(!copy_error, "corrupt fixture must be copied into the scan tree");

        const fs::path noise = root / L"notes.txt";
        write_noise_file(noise);

        mv::ImageIndex index;
        const int count = index.scan(root.wstring(), true);
        expect(count == 4,
            "recursive scan must index 3 BMPs plus the corrupt PNG fixture");
        expect(index.size() == 4,
            "index size must match the scan count");

        expect(index.index_of(bmp_level2.wstring()) >= 0,
            "recursive scan must reach the second nested subdirectory");
        expect(index.index_of(noise.wstring()) == -1,
            "non-image noise files must be excluded from the index");
        expect(index.index_of(corrupt_copy.wstring()) >= 0,
            "indexer must index the corrupt fixture as an image without decoding it");

        const std::vector<fs::path> bmp_paths = {bmp_root, bmp_level1, bmp_level2};
        const std::vector<std::pair<uint32_t, uint32_t>> bmp_sizes = {
            {8, 6}, {16, 10}, {24, 18}
        };
        for (size_t i = 0; i < bmp_paths.size(); ++i) {
            const int position = index.index_of(bmp_paths[i].wstring());
            expect(position >= 0, "indexed BMP must be present in the index");

            auto bitmap = decoder.decode_scaled(bmp_paths[i].wstring(), 160);
            uint32_t width = 0, height = 0;
            if (bitmap) bitmap->GetSize(&width, &height);
            expect(bitmap && width == bmp_sizes[i].first
                    && height == bmp_sizes[i].second,
                "decode_scaled must return the source dimensions for small BMPs");
        }

        bool corrupt_threw = false;
        try {
            (void)decoder.decode(corrupt_copy.wstring());
        } catch (const std::exception&) {
            corrupt_threw = true;
        }
        expect(corrupt_threw,
            "decoding the corrupt fixture must throw, proving the indexer never decodes");

        std::error_code remove_error;
        expect(fs::remove(bmp_root, remove_error) && !remove_error,
            "test must be able to delete a BMP from the scan tree");

        const int rescan_count = index.scan(root.wstring(), true);
        expect(rescan_count == 3,
            "rescan after deleting one BMP must decrement the indexed count");
        expect(index.index_of(bmp_root.wstring()) == -1,
            "rescan must drop the removed BMP path from the index");

        if (failures != 0) {
            std::cerr << failures << " assertion(s) failed\n";
            return 1;
        }
        std::cout << "pipeline_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "uncaught exception: " << error.what() << '\n';
        return 2;
    } catch (...) {
        std::cerr << "uncaught non-standard exception\n";
        return 2;
    }
}
