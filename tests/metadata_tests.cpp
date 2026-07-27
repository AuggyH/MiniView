#include "metadata.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::temp_directory_path() / (L"minview-metadata-tests-" + std::to_wstring(suffix));
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

void write_u32_be(std::ofstream& output, uint32_t value) {
    const std::array<unsigned char, 4> bytes = {
        static_cast<unsigned char>((value >> 24) & 0xff),
        static_cast<unsigned char>((value >> 16) & 0xff),
        static_cast<unsigned char>((value >> 8) & 0xff),
        static_cast<unsigned char>(value & 0xff)
    };
    output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void write_signature(std::ofstream& output) {
    constexpr std::array<unsigned char, 8> signature = {
        0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a
    };
    output.write(reinterpret_cast<const char*>(signature.data()), signature.size());
}

void write_chunk(std::ofstream& output, const char type[5], const std::string& data) {
    write_u32_be(output, static_cast<uint32_t>(data.size()));
    output.write(type, 4);
    output.write(data.data(), static_cast<std::streamsize>(data.size()));
    write_u32_be(output, 0);
}

} // namespace

int main() {
    TempDirectory temp;

    const fs::path valid = temp.path() / L"valid.png";
    {
        std::ofstream output(valid, std::ios::binary);
        write_signature(output);
        write_chunk(output, "tEXt", std::string("parameters\0", 11)
            + "cat\nNegative prompt: blur, Steps: 20, Sampler: Euler, CFG scale: 7, Seed: 42, Size: 512x512, Model: test");
        write_chunk(output, "IEND", "");
    }
    const mv::ImageMeta valid_meta = mv::extract_metadata(valid.wstring());
    expect(valid_meta.valid && valid_meta.steps == 20 && valid_meta.seed == 42,
        "bounded PNG parser should preserve normal tEXt metadata");

    const fs::path malicious = temp.path() / L"malicious.png";
    {
        std::ofstream output(malicious, std::ios::binary);
        write_signature(output);
        write_u32_be(output, 0x7fffffffu);
        output.write("tEXt", 4);
    }
    expect(!mv::extract_metadata(malicious.wstring()).valid,
        "declared chunk length beyond the file must be rejected without allocation");

    const fs::path truncated = temp.path() / L"truncated.png";
    {
        std::ofstream output(truncated, std::ios::binary);
        write_signature(output);
        write_u32_be(output, 16);
        output.write("tEXt", 4);
        output.write("x", 1);
    }
    expect(!mv::extract_metadata(truncated.wstring()).valid,
        "truncated text chunks must fail closed");

    const fs::path oversized = temp.path() / L"oversized.png";
    {
        std::ofstream output(oversized, std::ios::binary);
        write_signature(output);
        std::string data(4 * 1024 * 1024 + 1, 'x');
        write_chunk(output, "tEXt", data);
        write_chunk(output, "IEND", "");
    }
    expect(!mv::extract_metadata(oversized.wstring()).valid,
        "text chunks above the metadata budget must be skipped");

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }

    std::cout << "metadata tests passed\n";
    return 0;
}
