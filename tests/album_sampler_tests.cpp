// Unit tests for the pure album folder sampling model (Maintainability Phase 1).
// Covers path_under_folder boundaries and the 0/1/2/3/4+ sampling rule.

#include "album_sampler.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void test_path_boundary() {
    expect(mv::path_under_folder(L"D:\\A\\x.jpg", L"D:\\A"),
        "D:\\A contains D:\\A\\x.jpg");
    expect(!mv::path_under_folder(L"D:\\AIGC\\x.jpg", L"D:\\A"),
        "D:\\A does not contain D:\\AIGC\\x.jpg");
    expect(mv::path_under_folder(L"D:\\A", L"D:\\A"),
        "folder equals path matches");
    expect(!mv::path_under_folder(L"D:\\A", L"D:\\AB"),
        "shorter path is not under longer folder");
    expect(!mv::path_under_folder(L"", L"D:\\A"),
        "empty path is not under folder");
    expect(!mv::path_under_folder(L"D:\\A\\x.jpg", L""),
        "empty folder matches nothing");
}

void test_path_trailing_separators() {
    expect(mv::path_under_folder(L"D:\\A\\x.jpg", L"D:\\A\\"),
        "trailing backslash on folder is ignored");
    expect(mv::path_under_folder(L"D:\\A\\x.jpg", L"D:\\A/"),
        "trailing slash on folder is ignored");
    expect(mv::path_under_folder(L"D:/A/x.jpg", L"D:/A"),
        "forward-slash folder and path match");
    expect(!mv::path_under_folder(L"D:/A/x.jpg", L"D:\\A"),
        "mixed separators are not normalized (original behavior)");
    expect(mv::path_under_folder(L"d:\\a\\x.jpg", L"D:\\A"),
        "path comparison is case-insensitive");
}

std::vector<std::wstring> make_paths(const std::wstring& folder,
    size_t count) {
    std::vector<std::wstring> paths;
    for (size_t i = 0; i < count; ++i)
        paths.push_back(folder + L"\\img_" + std::to_wstring(i) + L".jpg");
    return paths;
}

void test_sample_counts() {
    const std::vector<std::wstring> folders = {L"D:\\A", L"D:\\B", L"D:\\C"};

    {
        // 0 per folder
        const auto samples = mv::sample_paths_per_folder({}, folders);
        expect(samples.size() == 3, "empty input: parallel result size");
        expect(samples[0].empty(), "empty input: folder A no samples");
        expect(samples[1].empty(), "empty input: folder B no samples");
        expect(samples[2].empty(), "empty input: folder C no samples");
    }
    {
        // 1 per folder
        auto paths = make_paths(L"D:\\A", 1);
        auto more = make_paths(L"D:\\B", 1);
        paths.insert(paths.end(), more.begin(), more.end());
        const auto samples = mv::sample_paths_per_folder(paths, folders);
        expect(samples[0].size() == 1, "1 path: folder A has 1 sample");
        expect(samples[0][0] == L"D:\\A\\img_0.jpg", "1 path: first sample");
        expect(samples[1].size() == 1, "1 path: folder B has 1 sample");
        expect(samples[2].empty(), "1 path: folder C no samples");
    }
    {
        // 2 per folder
        const auto samples = mv::sample_paths_per_folder(
            make_paths(L"D:\\A", 2), folders);
        expect(samples[0].size() == 2, "2 paths: both sampled");
        expect(samples[0][0] == L"D:\\A\\img_0.jpg", "2 paths: first");
        expect(samples[0][1] == L"D:\\A\\img_1.jpg", "2 paths: second");
    }
    {
        // 3 per folder
        const auto samples = mv::sample_paths_per_folder(
            make_paths(L"D:\\A", 3), folders);
        expect(samples[0].size() == 3, "3 paths: all sampled");
        expect(samples[0][2] == L"D:\\A\\img_2.jpg", "3 paths: third");
    }
    {
        // 4 per folder
        const auto samples = mv::sample_paths_per_folder(
            make_paths(L"D:\\A", 4), folders);
        expect(samples[0].size() == 4, "4 paths: 4 samples");
        expect(samples[0][0] == L"D:\\A\\img_0.jpg", "4 paths: sample 0");
        expect(samples[0][1] == L"D:\\A\\img_1.jpg", "4 paths: sample k/4");
        expect(samples[0][2] == L"D:\\A\\img_2.jpg", "4 paths: sample k/2");
        expect(samples[0][3] == L"D:\\A\\img_3.jpg", "4 paths: sample 3k/4");
    }
    {
        // 6 per folder: indices 0, 1, 3, 4
        const auto samples = mv::sample_paths_per_folder(
            make_paths(L"D:\\A", 6), folders);
        expect(samples[0].size() == 4, "6 paths: still 4 samples");
        expect(samples[0][0] == L"D:\\A\\img_0.jpg", "6 paths: sample 0");
        expect(samples[0][1] == L"D:\\A\\img_1.jpg", "6 paths: sample 6/4=1");
        expect(samples[0][2] == L"D:\\A\\img_3.jpg", "6 paths: sample 6/2=3");
        expect(samples[0][3] == L"D:\\A\\img_4.jpg",
            "6 paths: sample 3*6/4=4");
    }
}

void test_first_folder_wins() {
    {
        const std::vector<std::wstring> folders = {L"D:\\A", L"D:\\A\\B"};
        const std::vector<std::wstring> paths = {L"D:\\A\\B\\x.jpg"};
        const auto samples = mv::sample_paths_per_folder(paths, folders);
        expect(samples[0].size() == 1,
            "nested folder: parent first wins");
        expect(samples[0][0] == L"D:\\A\\B\\x.jpg",
            "nested folder: path assigned to parent");
        expect(samples[1].empty(), "nested folder: child not used");
    }
    {
        const std::vector<std::wstring> folders = {L"D:\\A\\B", L"D:\\A"};
        const std::vector<std::wstring> paths = {L"D:\\A\\B\\x.jpg"};
        const auto samples = mv::sample_paths_per_folder(paths, folders);
        expect(samples[0].size() == 1,
            "nested folder: child first wins");
        expect(samples[1].empty(), "nested folder: parent gets nothing");
    }
}

} // namespace

int main() {
    struct TestCase {
        const char* name;
        void (*fn)();
    } cases[] = {
        {"path_boundary", test_path_boundary},
        {"path_trailing_separators", test_path_trailing_separators},
        {"sample_counts", test_sample_counts},
        {"first_folder_wins", test_first_folder_wins},
    };
    int failures = 0;
    for (const auto& test : cases) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << "\n";
            ++failures;
        }
    }
    if (failures == 0) {
        std::cout << "album_sampler: all "
                  << sizeof(cases) / sizeof(cases[0]) << " tests passed\n";
        return 0;
    }
    std::cerr << "album_sampler: " << failures << " test(s) failed\n";
    return 1;
}
