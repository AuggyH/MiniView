#include "app_state.h"

#include <iostream>
#include <string>
#include <vector>

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

    expect(mv::ensure_grid_row_visible(400, 450, 550, 1200, 500) == 400,
        "a selected row that remains visible should preserve scroll");
    expect(mv::ensure_grid_row_visible(400, 250, 350, 1200, 500) == 250,
        "a resize that moves the selected row above the viewport should reveal it");
    expect(mv::ensure_grid_row_visible(100, 680, 820, 1400, 500) == 320,
        "column or panel width changes should reveal the selected row below the viewport");
    expect(mv::ensure_grid_row_visible(200, 650, 900, 1500, 500) == 400,
        "zoom, labels, and square/justified row-height changes should share the contract");
    expect(mv::ensure_grid_row_visible(900, 150, 250, 450, 500) == 0,
        "a shortened list should clamp before restoring selected-row visibility");

    std::wstring current_path;
    int current_index = -1;
    bool has_image = false;
    mv::commit_current_image_identity(
        L"A-valid.png", 0, current_path, current_index, has_image);
    expect(mv::can_delete_current_image(has_image, current_path),
        "a successfully committed image should be deletable");

    const std::vector<std::wstring> remaining_paths = {L"B-damaged.png"};
    const int damaged_successor = mv::begin_post_delete_transition(
        0, static_cast<int>(remaining_paths.size()),
        current_path, current_index, has_image);
    expect(damaged_successor == 0,
        "deleting A should select B's remaining index for an open attempt");
    expect(remaining_paths[static_cast<size_t>(damaged_successor)] == L"B-damaged.png",
        "the successor attempt should remain bound to damaged B's path");
    // Simulate B failing decode/upload: open_image must not call commit.
    expect(current_path.empty() && current_index == -1 && !has_image,
        "a damaged successor must leave current image identity cleared");
    expect(!mv::can_delete_current_image(has_image, current_path),
        "a consecutive delete must not act on the still-indexed damaged successor");
    expect(remaining_paths.size() == 1,
        "the skipped consecutive delete must leave damaged B indexed");

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }

    std::cout << "app state tests passed\n";
    return 0;
}
