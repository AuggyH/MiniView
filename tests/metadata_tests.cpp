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

void write_u16_le(std::ofstream& output, uint16_t value) {
    const std::array<unsigned char, 2> bytes = {
        static_cast<unsigned char>(value & 0xff),
        static_cast<unsigned char>((value >> 8) & 0xff)
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

// Writes a minimal JPEG with one APP1 (EXIF) segment and no image data.
// read_jpeg_comment only needs SOI + APP1 to reach the TIFF walk.
void write_jpeg_with_app1(const fs::path& path, const std::string& app1_payload) {
    std::ofstream output(path, std::ios::binary);
    output.put(static_cast<char>(0xff));
    output.put(static_cast<char>(0xd8));  // SOI
    output.put(static_cast<char>(0xff));
    output.put(static_cast<char>(0xe1));  // APP1
    const uint32_t segment_length =
        static_cast<uint32_t>(app1_payload.size()) + 2;
    output.put(static_cast<char>((segment_length >> 8) & 0xff));
    output.put(static_cast<char>(segment_length & 0xff));
    output.write(app1_payload.data(), static_cast<std::streamsize>(app1_payload.size()));
    output.put(static_cast<char>(0xff));
    output.put(static_cast<char>(0xd9));  // EOI
}

// Little-endian TIFF with one EXIF UserComment entry whose value starts at
// payload offset 34 and carries an 8-byte "ASCII\0\0\0\0\0" charset prefix.
std::string make_exif_payload(uint32_t ifd_off, uint16_t entry_count,
    const std::string& user_comment) {
    std::string data;
    data += std::string("Exif\0\0", 6);
    data += std::string("II", 2);
    data += std::string({'\x2a', '\x00'});              // TIFF magic (LE)
    data += std::string({
        static_cast<char>(ifd_off & 0xff),
        static_cast<char>((ifd_off >> 8) & 0xff),
        static_cast<char>((ifd_off >> 16) & 0xff),
        static_cast<char>((ifd_off >> 24) & 0xff)});
    data += std::string(6, '\0');                        // padding to IFD
    data += std::string({
        static_cast<char>(entry_count & 0xff),
        static_cast<char>((entry_count >> 8) & 0xff)});
    if (entry_count >= 1) {
        data += std::string({'\x86', '\x92'});           // tag 0x9286
        data += std::string({'\x07', '\x00'});           // type 7 = undefined
        const uint32_t count = 8 + static_cast<uint32_t>(user_comment.size());
        data += std::string({
            static_cast<char>(count & 0xff),
            static_cast<char>((count >> 8) & 0xff),
            static_cast<char>((count >> 16) & 0xff),
            static_cast<char>((count >> 24) & 0xff)});
        constexpr uint32_t value_offset = 34;
        data += std::string({
            static_cast<char>(value_offset & 0xff),
            static_cast<char>((value_offset >> 8) & 0xff),
            static_cast<char>((value_offset >> 16) & 0xff),
            static_cast<char>((value_offset >> 24) & 0xff)});
    }
    data += std::string("ASCII\0\0\0\0\0", 8);
    data += user_comment;
    return data;
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

    // JPEG EXIF UserComment round-trip (little-endian TIFF).
    const fs::path valid_jpg = temp.path() / L"valid.jpg";
    write_jpeg_with_app1(valid_jpg, make_exif_payload(14, 1,
        "cat\nNegative prompt: blur, Steps: 20, Sampler: Euler, "
        "CFG scale: 7, Seed: 42, Size: 512x512, Model: test"));
    const mv::ImageMeta jpg_meta = mv::extract_metadata(valid_jpg.wstring());
    expect(jpg_meta.valid && jpg_meta.steps == 20 && jpg_meta.seed == 42,
        "bounded JPEG EXIF reader should parse a valid UserComment");

    const fs::path bad_ifd = temp.path() / L"bad_ifd.jpg";
    write_jpeg_with_app1(bad_ifd, make_exif_payload(0xffffu, 1, "x"));
    expect(!mv::extract_metadata(bad_ifd.wstring()).valid,
        "an IFD offset beyond the segment must fail closed without OOB read");

    const fs::path bad_count = temp.path() / L"bad_count.jpg";
    write_jpeg_with_app1(bad_count, make_exif_payload(14, 0xffff, "x"));
    expect(!mv::extract_metadata(bad_count.wstring()).valid,
        "an IFD entry table larger than the segment must fail closed");

    // WebUI numeric fields must fail closed on overflow instead of throwing.
    const fs::path overflow = temp.path() / L"overflow.png";
    {
        std::ofstream output(overflow, std::ios::binary);
        write_signature(output);
        write_chunk(output, "tEXt", std::string("parameters\0", 11)
            + "cat\nSteps: 99999999999999999999, Seed: 1");
        write_chunk(output, "IEND", "");
    }
    expect(!mv::extract_metadata(overflow.wstring()).valid,
        "overflowing numeric metadata must fail the whole record closed");

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }

    std::cout << "metadata tests passed\n";
    return 0;
}
