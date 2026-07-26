#include "indexer.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
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
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        m_path = fs::temp_directory_path() / (L"minview-indexer-tests-" + std::to_wstring(suffix));
        fs::create_directories(m_path / L"nested");
    }

    ~TempDirectory() {
        std::error_code error;
        fs::remove_all(m_path, error);
    }

    const fs::path& path() const { return m_path; }

private:
    fs::path m_path;
};

void write_file(const fs::path& path, size_t size) {
    std::ofstream output(path, std::ios::binary);
    output << std::string(size, 'x');
}

std::wstring filename_at(const mv::ImageIndex& index, size_t position) {
    return fs::path(index.path_at(position)).filename().wstring();
}

} // namespace

int main() {
    TempDirectory temp;
    const fs::path root = temp.path();
    const fs::path alpha = root / L"a.png";
    const fs::path beta = root / L"B.JPG";
    const fs::path nested = root / L"nested" / L"c.GIF";

    write_file(alpha, 1);
    write_file(beta, 3);
    write_file(nested, 2);
    write_file(root / L"notes.txt", 5);

    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(alpha, now - std::chrono::seconds(3));
    fs::last_write_time(beta, now - std::chrono::seconds(2));
    fs::last_write_time(nested, now - std::chrono::seconds(1));

    mv::ImageIndex index;
    expect(index.scan(root.wstring(), false) == 2,
        "non-recursive scan should include only root image files");
    expect(filename_at(index, 0) == L"a.png" && filename_at(index, 1) == L"B.JPG",
        "name sort should be case-insensitive");
    expect(index.index_of(alpha.wstring()) == 0,
        "index lookup should match a scanned path");

    expect(index.scan(root.wstring(), true) == 3,
        "recursive scan should include nested image files");
    expect(index.index_of(nested.wstring()) >= 0,
        "recursive scan should index the nested image");
    const int nested_index = index.index_of(nested.wstring());
    expect(index.relpath_at(static_cast<size_t>(nested_index)) == L"nested\\c.GIF",
        "relative path should be rooted at the scanned directory");

    index.sort_by(mv::SortMode::Size);
    expect(filename_at(index, 0) == L"B.JPG" && filename_at(index, 2) == L"a.png",
        "size sort should order largest first");

    index.sort_by(mv::SortMode::Date);
    expect(filename_at(index, 0) == L"c.GIF" && filename_at(index, 2) == L"a.png",
        "date sort should order newest first");

    const int beta_index = index.index_of(beta.wstring());
    expect(index.remove(beta_index), "remove should accept a valid index");
    expect(index.size() == 2 && index.index_of(beta.wstring()) == -1,
        "remove should rebuild the path lookup");
    expect(!index.remove(-1), "remove should reject an invalid index");

    expect(index.scan((root / L"missing").wstring(), true) == -1,
        "scan should report an inaccessible directory");

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }

    std::cout << "indexer tests passed\n";
    return 0;
}
