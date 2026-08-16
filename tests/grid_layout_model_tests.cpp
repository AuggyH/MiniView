// Unit tests for the pure grid layout model (Maintainability Phase 1).
// Covers the arithmetic extracted from App::rebuild_grid_layout.

#include "grid_layout_model.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void expect_near(float actual, float expected, float tolerance,
    const char* message) {
    if (!std::isfinite(actual) || !std::isfinite(expected)
        || std::fabs(actual - expected) > tolerance) {
        throw std::runtime_error(message);
    }
}

using Dims = std::pair<uint32_t, uint32_t>;

mv::GridLayoutInput make_input(int count, int area_width, bool square,
    bool show_labels, float dpi_scale, const std::vector<Dims>& dims) {
    mv::GridLayoutInput input;
    input.item_count = count;
    input.area_width = area_width;
    input.cell = 200;
    input.gap_h = 8;
    input.gap_v = 16;
    input.pad = 8;
    input.square = square;
    input.show_labels = show_labels;
    input.dpi_scale = dpi_scale;
    input.dims = dims;
    return input;
}

void test_square_layout() {
    std::vector<Dims> dims(5, {2000, 1000});
    const mv::GridLayout plan =
        plan_grid_layout(make_input(5, 1000, true, true, 1.0f, dims));

    expect(plan.cols == 4, "square: 1000px area at cell 200 gives 4 columns");
    expect(plan.rows.size() == 2, "square: 5 items form 2 rows");
    expect(plan.rows[0].start_idx == 0, "square: first row starts at 0");
    expect(plan.rows[0].end_idx == 4, "square: first row ends at 4");
    expect(plan.rows[0].row_h == 244, "square: cell expands to 244");
    expect(plan.rows[0].row_y == 8, "square: first row starts at pad");
    expect(plan.rows[0].label_extra == 42, "square: 96dpi label height 42");
    expect(plan.rows[1].start_idx == 4, "square: second row starts at 4");
    expect(plan.rows[1].end_idx == 5, "square: second row has one item");
    expect(plan.rows[1].row_y == 310, "square: second row y after label+gap");
    expect_near(plan.item_x[0], 0.0f, 0.001f, "square: first item x 0");
    expect_near(plan.item_x[3], 3.0f * (244.0f + 8.0f), 0.001f,
        "square: fourth item x after 3 cells");
    expect_near(plan.item_x[4], 0.0f, 0.001f,
        "square: last-row item x starts at 0");
    expect_near(plan.item_w[0], 244.0f, 0.001f, "square: item width");
    expect_near(plan.item_w[4], 244.0f, 0.001f,
        "square: last-row item width same cell");
    expect(plan.total_height == 612, "square: total height includes pad/gaps");
}

void test_square_centers_x0() {
    std::vector<Dims> dims(3, {2000, 1000});
    const mv::GridLayout plan =
        plan_grid_layout(make_input(3, 810, true, true, 1.0f, dims));

    expect(plan.cols == 3, "square centered: 810px area gives 3 columns");
    // cell_width = max(200, (810 - 2*8)/3) = max(200, 264) = 264
    // x0 = (810 - 3*264 - 2*8)/2 = 1
    expect(plan.rows[0].row_h == 264, "square centered: expanded cell 264");
    expect_near(plan.item_x[0], 1.0f, 0.001f,
        "square centered: row is centered via x0=1");
}

void test_justified_layout() {
    std::vector<Dims> dims = {
        {3000, 1000},  // 3:1 wide
        {1000, 1000},  // 1:1
        {1000, 2000},  // 1:2 tall
    };
    const mv::GridLayout plan =
        plan_grid_layout(make_input(3, 700, false, true, 1.0f, dims));

    expect(plan.cols == 3, "justified: 700px area gives 3 columns");
    expect(plan.rows.size() == 1, "justified: 3 items fit one row");
    expect(plan.rows[0].row_h == 152, "justified: aspect-scaled row height");
    expect(plan.rows[0].label_extra == 42, "justified: label extra present");
    expect_near(plan.item_x[0], 0.0f, 0.001f, "justified: first x 0");
    expect_near(plan.item_x[1], 456.0f + 8.0f, 0.001f,
        "justified: second x after wide item");
    expect_near(plan.item_x[2], 456.0f + 8.0f + 152.0f + 8.0f, 0.001f,
        "justified: third x after square item");
    expect_near(plan.item_w[0], 456.0f, 0.001f,
        "justified: wide item display width");
    expect_near(plan.item_w[1], 152.0f, 0.001f,
        "justified: square item display width");
    expect_near(plan.item_w[2], 76.0f, 0.001f,
        "justified: tall item display width");
    expect(plan.total_height == 218, "justified: total height includes label");
}

void test_justified_min_row_40() {
    std::vector<Dims> dims(4, {10, 1});
    const mv::GridLayout plan =
        plan_grid_layout(make_input(4, 1000, false, false, 1.0f, dims));

    expect(plan.cols == 4, "justified min row: 4 columns");
    expect(plan.rows[0].row_h == 40,
        "justified min row: very wide images clamp to row height 40");
    expect_near(plan.item_w[0], 400.0f, 0.001f,
        "justified min row: width follows clamped 40px height");
    expect(plan.total_height == 64, "justified min row: total height 8+40+16");
}

void test_zero_items() {
    std::vector<Dims> dims;
    const mv::GridLayout plan =
        plan_grid_layout(make_input(0, 1000, false, true, 1.0f, dims));

    expect(plan.cols == 4, "zero items: columns still computed");
    expect(plan.rows.empty(), "zero items: no rows");
    expect(plan.item_x.empty(), "zero items: no item x positions");
    expect(plan.item_w.empty(), "zero items: no item widths");
    expect(plan.total_height == 8, "zero items: total height is the pad");
}

void test_zero_dims_fallback() {
    std::vector<Dims> dims(2, {0, 0});
    const mv::GridLayout plan =
        plan_grid_layout(make_input(2, 600, false, false, 1.0f, dims));

    expect(plan.cols == 2, "zero dims: 600px area gives 2 columns");
    expect(plan.rows[0].row_h == 296,
        "zero dims: 0x0 falls back to 1x1; row scales to usable width");
    expect_near(plan.item_w[0], 296.0f, 0.001f,
        "zero dims: fallback square display width");
}

void test_label_height_on_off() {
    std::vector<Dims> dims(5, {2000, 1000});
    const mv::GridLayout labels_on =
        plan_grid_layout(make_input(5, 1000, true, true, 1.0f, dims));
    const mv::GridLayout labels_off =
        plan_grid_layout(make_input(5, 1000, true, false, 1.0f, dims));

    expect(labels_on.rows[0].label_extra == 42,
        "labels on: label extra 42 at 96dpi");
    expect(labels_off.rows[0].label_extra == 0,
        "labels off: label extra 0");
    expect(labels_on.total_height == 612,
        "labels on: total height includes 2 label blocks");
    expect(labels_off.total_height == 528,
        "labels off: total height excludes label blocks");
}

void test_label_height_dpi_scaled() {
    std::vector<Dims> dims(2, {2000, 1000});
    const mv::GridLayout plan =
        plan_grid_layout(make_input(2, 1000, true, true, 2.0f, dims));

    expect(plan.rows[0].label_extra == 84,
        "192dpi: label height 42 * 2.0");
}

void test_padding_offset() {
    std::vector<Dims> dims(1, {2000, 1000});
    mv::GridLayoutInput input =
        make_input(1, 1000, true, false, 1.0f, dims);
    input.pad = 23;
    const mv::GridLayout plan = plan_grid_layout(input);

    expect(plan.rows[0].row_y == 23, "padding: first row y equals pad");
    expect(plan.total_height == 23 + 244 + 16,
        "padding: total height starts from pad");
}

} // namespace

int main() {
    struct TestCase {
        const char* name;
        void (*fn)();
    } cases[] = {
        {"square_layout", test_square_layout},
        {"square_centers_x0", test_square_centers_x0},
        {"justified_layout", test_justified_layout},
        {"justified_min_row_40", test_justified_min_row_40},
        {"zero_items", test_zero_items},
        {"zero_dims_fallback", test_zero_dims_fallback},
        {"label_height_on_off", test_label_height_on_off},
        {"label_height_dpi_scaled", test_label_height_dpi_scaled},
        {"padding_offset", test_padding_offset},
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
        std::cout << "grid_layout_model: all "
                  << sizeof(cases) / sizeof(cases[0]) << " tests passed\n";
        return 0;
    }
    std::cerr << "grid_layout_model: " << failures << " test(s) failed\n";
    return 1;
}
