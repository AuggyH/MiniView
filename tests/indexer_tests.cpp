#include "indexer.h"

#include <Windows.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <cwctype>

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

bool is_ntfs_volume(const fs::path& path) {
    wchar_t volume_root[MAX_PATH]{};
    wchar_t file_system[MAX_PATH]{};
    if (!GetVolumePathNameW(path.c_str(), volume_root, MAX_PATH)) return false;
    if (!GetVolumeInformationW(volume_root, nullptr, 0, nullptr, nullptr, nullptr,
            file_system, MAX_PATH)) return false;
    return CompareStringOrdinal(file_system, -1, L"NTFS", -1, TRUE) == CSTR_EQUAL;
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
    std::wstring alternate_alpha = alpha.wstring();
    for (auto& ch : alternate_alpha) {
        if (ch == L'\\') ch = L'/';
        ch = static_cast<wchar_t>(towupper(ch));
    }
    expect(index.index_of(alternate_alpha) == 0,
        "Windows path lookup should ignore case and slash direction");

    expect(index.scan(root.wstring(), true) == 3,
        "recursive scan should include nested image files");
    expect(index.index_of(nested.wstring()) >= 0,
        "recursive scan should index the nested image");
    const int nested_index = index.index_of(nested.wstring());
    expect(index.relpath_at(static_cast<size_t>(nested_index)) == L"nested\\c.GIF",
        "relative path should be rooted at the scanned directory");

    {
        TempDirectory empty_root_fixture;
        const fs::path empty_root =
            empty_root_fixture.path() / L"中文空根目录";
        const fs::path child_image = empty_root / L"子目录甲" / L"图片甲.png";
        const fs::path grandchild_image =
            empty_root / L"子目录乙" / L"孙目录" / L"图片乙.jpg";
        fs::create_directories(child_image.parent_path());
        fs::create_directories(grandchild_image.parent_path());
        write_file(child_image, 4);
        write_file(grandchild_image, 5);

        mv::ImageIndex empty_root_index;
        expect(empty_root_index.scan(empty_root.wstring(), false) == 0
                && empty_root_index.directory() == empty_root.wstring(),
            "a non-recursive empty root must remain bound as the browse root");
        expect(empty_root_index.scan(empty_root.wstring(), true) == 2
                && empty_root_index.directory() == empty_root.wstring()
                && empty_root_index.index_of(child_image.wstring()) >= 0
                && empty_root_index.index_of(grandchild_image.wstring()) >= 0,
            "recursive scanning must find Chinese child and grandchild images under the same root");
    }

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

    expect(index.scan(root.wstring(), true) == 3,
        "batch removal fixture should restore all indexed files");
    const int alpha_index = index.index_of(alpha.wstring());
    const int nested_batch_index = index.index_of(nested.wstring());
    expect(index.remove_many({alpha_index, nested_batch_index, alpha_index, -1, 99}) == 2,
        "batch remove should ignore duplicates and invalid indices");
    expect(index.size() == 1 && index.index_of(alpha.wstring()) == -1
            && index.index_of(nested.wstring()) == -1,
        "batch remove should compact entries and rebuild the path map once");

    expect(index.scan((root / L"missing").wstring(), true) == -1,
        "scan should report an inaccessible directory");

    fs::path unicode_fixture_path;
    {
        TempDirectory unicode_fixture;
        unicode_fixture_path = unicode_fixture.path();
        const fs::path upper_path = unicode_fixture_path / L"\u00C4.PNG";
        const fs::path lower_alias = unicode_fixture_path / L"\u00E4.png";
        write_file(upper_path, 4);

        expect(is_ntfs_volume(unicode_fixture_path),
            "the Unicode path identity fixture must run on NTFS");
        std::error_code identity_error;
        expect(fs::equivalent(upper_path, lower_alias, identity_error) && !identity_error,
            "uppercase and lowercase Unicode paths should address the same NTFS file");

        mv::ImageIndex unicode_index;
        expect(unicode_index.scan(unicode_fixture_path.wstring(), false) == 1,
            "the Unicode path fixture should contain one indexed image");
        std::wstring slash_alias = lower_alias.wstring();
        std::replace(slash_alias.begin(), slash_alias.end(), L'\\', L'/');
        expect(unicode_index.index_of(lower_alias.wstring()) == 0,
            "Windows invariant case folding should match the lowercase Unicode alias");
        expect(unicode_index.index_of(slash_alias) == 0,
            "Unicode path identity should also normalize slash direction");
    }
    expect(!fs::exists(unicode_fixture_path),
        "the isolated Unicode path fixture should be removed after the test");

    // ── 多根合并扫描(相册) ──
    {
        TempDirectory multi;
        const fs::path mroot = multi.path();
        const fs::path root_a = mroot / L"album-a";
        const fs::path root_b = mroot / L"album-b";
        fs::create_directories(root_a / L"sub");
        fs::create_directories(root_b);
        write_file(root_a / L"a1.png", 1);
        write_file(root_a / L"sub" / L"a2.png", 2);
        write_file(root_a / L"a3.png", 1);
        write_file(root_b / L"b1.jpg", 3);

        mv::ImageIndex multi_index;
        const std::vector<mv::ScanRoot> roots{
            {root_a.wstring(), true},
            {root_b.wstring(), false},
            {root_a.wstring(), false},   // 重复根 → 不重复计数
        };
        const int count = multi_index.scan_many(roots);
        expect(count == 4,
            "scan_many should merge roots and dedupe overlapping paths");
        expect(multi_index.directory().empty(),
            "multi-root index has no single root directory");
        expect(multi_index.index_of((root_a / L"sub" / L"a2.png").wstring()) >= 0,
            "recursive root content indexed");
        expect(multi_index.index_of((root_b / L"b1.jpg").wstring()) >= 0,
            "flat root content indexed");
        expect(multi_index.size() == 4, "no duplicates across roots");

        // 不可读根被跳过,不失败
        const std::vector<mv::ScanRoot> bad_roots{
            {(mroot / L"missing").wstring(), true},
            {root_b.wstring(), false},
        };
        mv::ImageIndex tolerant_index;
        expect(tolerant_index.scan_many(bad_roots) == 1,
            "unreadable roots are skipped in scan_many");
    }

    // ── 显式路径列表(固定收藏相册) ──
    {
        TempDirectory fav;
        const fs::path froot = fav.path();
        write_file(froot / L"f1.png", 1);
        write_file(froot / L"f2.jpg", 2);
        write_file(froot / L"gone.png", 1);
        const fs::path gone = froot / L"gone.png";
        fs::remove(gone);

        mv::ImageIndex fav_index;
        const std::vector<std::wstring> paths{
            (froot / L"f1.png").wstring(),
            (froot / L"f2.jpg").wstring(),
            gone.wstring(),                    // 已删除 → 跳过
            (froot / L"f1.png").wstring(),     // 重复 → 去重
        };
        const int count = fav_index.load_paths(paths);
        expect(count == 2,
            "load_paths should skip missing paths and dedupe");
        expect(fav_index.index_of((froot / L"f2.jpg").wstring()) >= 0,
            "explicit path indexed");
        expect(fav_index.directory().empty(),
            "path-list index has no root directory");
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }

    std::cout << "indexer tests passed\n";
    return 0;
}
