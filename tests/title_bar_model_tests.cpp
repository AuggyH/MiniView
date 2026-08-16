// Unit tests for the pure title-bar geometry model (Maintainability Phase 1).
// Covers button zones at 96/192 DPI, menu bounds, and hit-test boundaries.

#include "title_bar_model.h"

#include <cmath>
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

void test_button_zones_96dpi() {
    const mv::TitleBarLayout title{0.0f, 1000.0f, 40.0f, 1.0f};
    expect_near(title_bar_button_width(title), 46.0f, 0.001f,
        "96dpi: button width 46");
    expect(title_bar_window_button_at(title, 954.0f) == 2,
        "96dpi: close left edge at 1000-46");
    expect(title_bar_window_button_at(title, 999.0f) == 2,
        "96dpi: close interior");
    expect(title_bar_window_button_at(title, 953.9f) == 1,
        "96dpi: maximize starts immediately left of close");
    expect(title_bar_window_button_at(title, 908.0f) == 1,
        "96dpi: maximize left edge at 1000-92");
    expect(title_bar_window_button_at(title, 907.9f) == 0,
        "96dpi: minimize starts immediately left of maximize");
    expect(title_bar_window_button_at(title, 862.0f) == 0,
        "96dpi: minimize left edge at 1000-138");
    expect(title_bar_window_button_at(title, 861.9f) == -1,
        "96dpi: left of minimize is not a button");
}

void test_button_zones_192dpi() {
    const mv::TitleBarLayout title{0.0f, 2000.0f, 80.0f, 2.0f};
    expect_near(title_bar_button_width(title), 92.0f, 0.001f,
        "192dpi: button width 92");
    expect(title_bar_window_button_at(title, 1908.0f) == 2,
        "192dpi: close left edge at 2000-92");
    expect(title_bar_window_button_at(title, 1816.0f) == 1,
        "192dpi: maximize left edge at 2000-184");
    expect(title_bar_window_button_at(title, 1724.0f) == 0,
        "192dpi: minimize left edge at 2000-276");
    expect(title_bar_window_button_at(title, 1723.9f) == -1,
        "192dpi: left of minimize is not a button");
}

void test_contains_y_boundaries() {
    const mv::TitleBarLayout title{0.0f, 1000.0f, 40.0f, 1.0f};
    expect(title_bar_contains_y(title, 0.0f), "y=0 is inside");
    expect(title_bar_contains_y(title, 39.9f), "y just below height is inside");
    expect(!title_bar_contains_y(title, 40.0f), "y==height is outside");
    expect(!title_bar_contains_y(title, -0.1f), "negative y is outside");
}

void test_menu_bounds_96dpi() {
    const mv::TitleBarLayout title{0.0f, 1000.0f, 40.0f, 1.0f};
    const std::vector<float> text_widths = {30.0f, 40.0f, 50.0f};
    const auto bounds = title_bar_menu_bounds(title, text_widths);

    expect(bounds.size() == 3, "96dpi: 3 menu bounds");
    expect_near(bounds[0].left, 84.0f, 0.001f,
        "96dpi: first menu starts at pad+title+gap = 12+68+4");
    expect_near(bounds[0].right, 84.0f + 30.0f + 16.0f, 0.001f,
        "96dpi: first menu width = text + 16 pad");
    expect_near(bounds[1].left, 130.0f, 0.001f,
        "96dpi: second menu starts after first");
    expect_near(bounds[1].right, 186.0f, 0.001f,
        "96dpi: second menu right edge");
    expect_near(bounds[2].left, 186.0f, 0.001f,
        "96dpi: third menu starts after second");
    expect_near(bounds[2].right, 252.0f, 0.001f,
        "96dpi: third menu right edge");
}

void test_menu_bounds_192dpi() {
    const mv::TitleBarLayout title{0.0f, 2000.0f, 80.0f, 2.0f};
    const std::vector<float> text_widths = {30.0f, 40.0f, 50.0f};
    const auto bounds = title_bar_menu_bounds(title, text_widths);

    expect_near(bounds[0].left, 168.0f, 0.001f,
        "192dpi: first menu starts at (12+68+4)*2");
    expect_near(bounds[0].right, 168.0f + 30.0f + 32.0f, 0.001f,
        "192dpi: first menu width = text + 32 pad");
    expect_near(bounds[1].left, 230.0f, 0.001f,
        "192dpi: second menu starts");
    expect_near(bounds[2].left, 302.0f, 0.001f,
        "192dpi: third menu starts");
    expect_near(bounds[2].right, 384.0f, 0.001f,
        "192dpi: third menu right edge");
}

void test_menu_hit_test_boundaries() {
    const mv::TitleBarLayout title{0.0f, 1000.0f, 40.0f, 1.0f};
    const std::vector<float> text_widths = {30.0f, 40.0f, 50.0f};

    expect(title_bar_menu_item_at(title, 83.9f, text_widths) == -1,
        "float hit-test: left of first menu misses");
    expect(title_bar_menu_item_at(title, 84.0f, text_widths) == 0,
        "float hit-test: first menu left edge hits");
    expect(title_bar_menu_item_at(title, 129.9f, text_widths) == 0,
        "float hit-test: first menu right-open edge");
    expect(title_bar_menu_item_at(title, 130.0f, text_widths) == 1,
        "float hit-test: second menu left edge hits");
    expect(title_bar_menu_item_at(title, 251.9f, text_widths) == 2,
        "float hit-test: third menu right-open edge");
    expect(title_bar_menu_item_at(title, 252.0f, text_widths) == -1,
        "float hit-test: right of last menu misses");
}

void test_integral_menu_hit_preserves_truncation() {
    // At 1.1x the menu boundaries are fractional; the original WM_LBUTTONDOWN
    // and WM_MOUSEMOVE paths compared against static_cast<int>(left/right).
    const mv::TitleBarLayout title{0.0f, 1000.0f, 44.0f, 1.1f};
    const std::vector<float> text_widths = {30.0f, 40.0f};
    // menu start = (12+68+4)*1.1 = 92.4
    // item0 right = 92.4 + (30 + 16*1.1 = 47.6) = 140.0
    // item1 right = 140.0 + (40 + 17.6 = 57.6) = 197.6
    expect(title_bar_menu_item_at_integral(title, 92, text_widths) == 0,
        "integral hit-test: x=92 hits via truncated left (92.4 -> 92)");
    expect(title_bar_menu_item_at(title, 92.0f, text_widths) == -1,
        "float hit-test: x=92 misses fractional left edge");
    expect(title_bar_menu_item_at_integral(title, 139, text_widths) == 0,
        "integral hit-test: x=139 hits first menu");
    expect(title_bar_menu_item_at_integral(title, 140, text_widths) == 1,
        "integral hit-test: x=140 hits second menu");
    expect(title_bar_menu_item_at_integral(title, 196, text_widths) == 1,
        "integral hit-test: x=196 hits second menu");
    expect(title_bar_menu_item_at_integral(title, 197, text_widths) == -1,
        "integral hit-test: x=197 misses truncated right edge");
}

} // namespace

int main() {
    struct TestCase {
        const char* name;
        void (*fn)();
    } cases[] = {
        {"button_zones_96dpi", test_button_zones_96dpi},
        {"button_zones_192dpi", test_button_zones_192dpi},
        {"contains_y_boundaries", test_contains_y_boundaries},
        {"menu_bounds_96dpi", test_menu_bounds_96dpi},
        {"menu_bounds_192dpi", test_menu_bounds_192dpi},
        {"menu_hit_test_boundaries", test_menu_hit_test_boundaries},
        {"integral_menu_hit_preserves_truncation",
            test_integral_menu_hit_preserves_truncation},
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
        std::cout << "title_bar_model: all "
                  << sizeof(cases) / sizeof(cases[0]) << " tests passed\n";
        return 0;
    }
    std::cerr << "title_bar_model: " << failures << " test(s) failed\n";
    return 1;
}
