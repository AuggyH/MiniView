#include "renderer_state.h"

#include <cmath>
#include <d2d1.h>
#include <dxgi.h>
#include <iostream>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool nearly_equal(float left, float right, float tolerance = 0.01f) {
    return std::abs(left - right) <= tolerance;
}

} // namespace

int main() {
    expect(!mv::should_recreate_render_device(S_OK),
        "successful rendering must keep the current device");
    expect(mv::should_recreate_render_device(D2DERR_RECREATE_TARGET),
        "D2D target loss must recreate the renderer");
    expect(mv::should_recreate_render_device(DXGI_ERROR_DEVICE_REMOVED),
        "DXGI device removal must recreate the renderer");
    expect(mv::should_recreate_render_device(DXGI_ERROR_DEVICE_RESET),
        "DXGI device reset must recreate the renderer");
    expect(mv::should_recreate_render_device(E_FAIL),
        "unclassified failed Present or Resize results must fail closed");

    expect(mv::renderer_generation_changed(1, 2),
        "App caches must be invalidated after renderer recreation");
    expect(!mv::renderer_generation_changed(2, 2),
        "App caches should remain valid within one renderer generation");

    // Deterministic metric fixtures represent DWrite results for short Chinese,
    // long Chinese, and English/path feedback without depending on installed fonts.
    auto expect_scaled_short_chinese = [](float dpi) {
        const float scale = dpi / 96.0f;
        const auto layout = mv::calculate_panel_toast_layout({
            20.0f, 30.0f, 280.0f * scale, 400.0f * scale,
            44.0f * scale, 15.0f * scale, 1, dpi});
        expect(layout.single_line,
            "short Chinese feedback must remain on one line");
        expect(nearly_equal(layout.bounds.right - layout.bounds.left,
                96.0f * scale),
            "short Chinese feedback must use the DPI-scaled minimum width");
        expect(nearly_equal(layout.text_bounds.left - layout.bounds.left,
                10.0f * scale)
                && nearly_equal(layout.text_bounds.top - layout.bounds.top,
                    7.0f * scale),
            "toast text padding must scale with DPI");
        expect(nearly_equal(layout.corner_radius, 4.0f * scale),
            "toast corner radius must scale with DPI");
    };
    expect_scaled_short_chinese(96.0f);
    expect_scaled_short_chinese(168.0f);
    expect_scaled_short_chinese(192.0f);

    const auto long_chinese = mv::calculate_panel_toast_layout({
        10.0f, 20.0f, 490.0f, 700.0f,
        600.0f, 84.0f, 3, 168.0f});
    expect(!long_chinese.single_line,
        "long Chinese feedback must retain its measured multi-line layout");
    expect(nearly_equal(long_chinese.bounds.right - long_chinese.bounds.left,
            406.0f),
        "long Chinese feedback must stop at the panel-relative maximum width");
    expect(nearly_equal(long_chinese.bounds.bottom, 678.0f)
            && nearly_equal(long_chinese.bounds.top, 569.5f),
        "multi-line feedback must grow upward from the DPI-scaled bottom inset");

    const auto english_path = mv::calculate_panel_toast_layout({
        0.0f, 0.0f, 560.0f, 500.0f,
        250.0f, 30.0f, 1, 192.0f});
    expect(english_path.single_line
            && nearly_equal(english_path.bounds.right
                - english_path.bounds.left, 294.0f),
        "an English path that fits must keep a measured single-line width");

    const auto narrow_path = mv::calculate_panel_toast_layout({
        50.0f, 60.0f, 240.0f, 160.0f,
        800.0f, 80.0f, 4, 192.0f});
    expect(!narrow_path.single_line
            && nearly_equal(narrow_path.bounds.left, 98.0f)
            && nearly_equal(narrow_path.bounds.right, 242.0f),
        "a long path in a narrow panel must use the clamped maximum width");
    expect(nearly_equal(narrow_path.bounds.top, 108.0f)
            && nearly_equal(narrow_path.bounds.bottom, 172.0f)
            && nearly_equal(narrow_path.text_bounds.bottom
                - narrow_path.text_bounds.top, 36.0f),
        "a narrow panel must clamp multi-line height inside its visible area");
    expect(narrow_path.bounds.left >= 50.0f
            && narrow_path.bounds.right <= 290.0f
            && narrow_path.bounds.top >= 60.0f
            && narrow_path.bounds.bottom <= 220.0f,
        "toast geometry must remain inside an offset panel viewport");

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }

    std::cout << "renderer state tests passed\n";
    return 0;
}
