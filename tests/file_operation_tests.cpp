#include "file_operation.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

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
        m_path = fs::temp_directory_path() / (L"minview-delete-tests-" + std::to_wstring(suffix));
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

} // namespace

int main() {
    TempDirectory temp;
    const fs::path existing = temp.path() / L"existing.png";
    {
        std::ofstream output(existing, std::ios::binary);
        output << 'x';
    }

    expect(!mv::path_is_confirmed_missing(existing.wstring()),
        "an existing file must remain in the index");
    expect(mv::path_is_confirmed_missing((temp.path() / L"missing.png").wstring()),
        "a file-not-found postcondition should remove the index entry");

    expect(mv::delete_fully_completed(0, false, 3, 3),
        "all requested paths plus a successful Shell result is complete");
    expect(!mv::delete_fully_completed(0, true, 3, 3),
        "an aborted operation must remain visibly incomplete");
    expect(!mv::delete_fully_completed(1, false, 3, 1),
        "partial completion must be reported even when one path was removed");

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }

    std::cout << "file operation tests passed\n";
    return 0;
}
