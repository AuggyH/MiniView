// Unit tests for the pure selection-remapping helpers (Maintainability Phase 1).
// Uses a small real temp directory so ImageIndex can scan and re-sort it.

#include "selection_remap.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct TempIndex {
    std::filesystem::path dir;
    mv::ImageIndex index;

    explicit TempIndex() {
        namespace fs = std::filesystem;
        dir = fs::temp_directory_path() / L"minview_sel_remap_tests";
        std::error_code error;
        fs::remove_all(dir, error);
        error.clear();
        expect(fs::create_directory(dir, error) && !error,
            "temp dir created");
        const wchar_t* names[] = {L"a.jpg", L"b.png", L"c.gif", L"d.webp"};
        for (const auto* name : names) {
            std::ofstream file(dir / name, std::ios::binary);
            expect(file.good(), "temp image file created");
        }
        expect(index.scan(dir.wstring(), false) == 4,
            "temp index scans 4 files");
    }

    ~TempIndex() {
        std::error_code error;
        std::filesystem::remove_all(dir, error);
    }

    std::wstring path_of(const wchar_t* name) const {
        return (dir / name).wstring();
    }
};

void test_empty_index_remap() {
    mv::ImageIndex empty;
    const std::vector<std::wstring> paths = {L"C:\\missing\\a.jpg"};
    const std::vector<int> indices = remap_paths_to_indices(empty, paths);
    expect(indices.size() == 1, "empty index: result size preserved");
    expect(indices[0] == -1, "empty index: path maps to -1");
}

void test_missing_paths_and_order() {
    TempIndex data;
    const std::vector<std::wstring> paths = {
        data.path_of(L"c.gif"),
        data.path_of(L"a.jpg"),
        L"C:\\missing\\x.png",
        data.path_of(L"b.png"),
    };
    const std::vector<int> indices = remap_paths_to_indices(data.index, paths);
    expect(indices.size() == 4, "remap: result size equals path count");
    expect(indices[0] == 2, "remap: c.gif maps to sorted index 2");
    expect(indices[1] == 0, "remap: a.jpg maps to sorted index 0");
    expect(indices[2] == -1, "remap: missing path maps to -1");
    expect(indices[3] == 1, "remap: b.png maps to sorted index 1");
}

void test_plan_selection_remap() {
    TempIndex data;
    const std::vector<std::wstring> selected_before = {
        data.path_of(L"c.gif"),
        L"C:\\missing\\x.png",
        data.path_of(L"b.png"),
    };
    const mv::SelectionRemap remap = plan_selection_remap(
        data.index, selected_before, data.path_of(L"a.jpg"));

    expect(remap.grid_sel == 0, "plan: selected path remaps to index 0");
    expect(remap.anchor == 0, "plan: anchor follows selected path");
    expect(remap.selected.size() == 2, "plan: missing paths are dropped");
    expect(remap.selected[0] == 2, "plan: first surviving selection is c.gif");
    expect(remap.selected[1] == 1, "plan: second surviving selection is b.png");
}

void test_plan_selection_remap_empty_selected_path() {
    TempIndex data;
    const std::vector<std::wstring> selected_before = {
        data.path_of(L"a.jpg"),
        data.path_of(L"d.webp"),
    };
    const mv::SelectionRemap remap = plan_selection_remap(
        data.index, selected_before, L"");

    expect(remap.grid_sel == -1, "plan: empty selected path gives -1 grid sel");
    expect(remap.anchor == -1, "plan: anchor follows -1 grid sel");
    expect(remap.selected.size() == 2,
        "plan: selections still remap with empty selected path");
    expect(remap.selected[0] == 0, "plan: first selection remaps");
    expect(remap.selected[1] == 3, "plan: second selection remaps");
}

void test_plan_selection_remap_all_missing() {
    TempIndex data;
    const std::vector<std::wstring> selected_before = {
        L"C:\\missing\\a.jpg",
        L"C:\\missing\\b.png",
    };
    const mv::SelectionRemap remap = plan_selection_remap(
        data.index, selected_before, L"C:\\missing\\a.jpg");

    expect(remap.grid_sel == -1, "plan: missing selected path gives -1");
    expect(remap.anchor == -1, "plan: anchor -1 when selected path missing");
    expect(remap.selected.empty(), "plan: all-missing selections drop all");
}

} // namespace

int main() {
    struct TestCase {
        const char* name;
        void (*fn)();
    } cases[] = {
        {"empty_index_remap", test_empty_index_remap},
        {"missing_paths_and_order", test_missing_paths_and_order},
        {"plan_selection_remap", test_plan_selection_remap},
        {"plan_selection_remap_empty_selected_path",
            test_plan_selection_remap_empty_selected_path},
        {"plan_selection_remap_all_missing",
            test_plan_selection_remap_all_missing},
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
        std::cout << "selection_remap: all "
                  << sizeof(cases) / sizeof(cases[0]) << " tests passed\n";
        return 0;
    }
    std::cerr << "selection_remap: " << failures << " test(s) failed\n";
    return 1;
}
