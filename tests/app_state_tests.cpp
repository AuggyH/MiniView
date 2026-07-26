#include "app_state.h"

#include <iostream>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace

int main() {
    expect(mv::is_image_zoomed(2.0f, 1.0f),
        "ordinary wheel input should preserve an existing zoomed state");
    expect(!mv::is_image_zoomed(1.01f, 1.0f),
        "fit-scale tolerance should remain eligible for navigation");

    expect(mv::should_preserve_selection_for_drag(true, false, false),
        "dragging an already selected item should preserve the selection");
    expect(!mv::should_preserve_selection_for_drag(false, false, false),
        "dragging an unselected item should establish a new selection");
    expect(!mv::should_preserve_selection_for_drag(true, true, false),
        "shift click should keep range-selection semantics");

    expect(mv::clamp_grid_scroll_position(900, 600, 400) == 200,
        "shorter layouts should clamp scroll position to the new bottom");
    expect(mv::clamp_grid_scroll_position(-10, 600, 400) == 0,
        "scroll position should never be negative");
    expect(mv::clamp_grid_scroll_position(100, 300, 400) == 0,
        "content shorter than the viewport should reset scrolling");

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }

    std::cout << "app state tests passed\n";
    return 0;
}
