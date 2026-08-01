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

    std::vector<bool> selected(3, false);
    int grid_selection = -1;
    int selection_anchor = -1;
    expect(mv::apply_grid_item_selection(
            0, 3, false, false, selected, grid_selection, selection_anchor)
            && selected == std::vector<bool>({true, false, false})
            && grid_selection == 0 && selection_anchor == 0,
        "plain click must establish the exact internal selection");
    expect(mv::apply_grid_item_selection(
            1, 3, false, true, selected, grid_selection, selection_anchor)
            && mv::apply_grid_item_selection(
                2, 3, false, true, selected, grid_selection, selection_anchor)
            && selected == std::vector<bool>({true, true, true}),
        "Ctrl click must add each exact item to the internal selection");
    expect(mv::apply_grid_item_selection(
            2, 3, false, true, selected, grid_selection, selection_anchor)
            && selected == std::vector<bool>({true, true, false})
            && grid_selection == 2 && selection_anchor == 2,
        "Ctrl toggle off must retain focus without retaining selection");
    expect(mv::grid_item_has_selection_border(0, grid_selection, selected)
            && mv::grid_item_has_selection_border(1, grid_selection, selected)
            && !mv::grid_item_has_selection_border(2, grid_selection, selected),
        "only exact internal selection members may render selection borders");

    selected.assign(3, false);
    grid_selection = -1;
    selection_anchor = -1;
    expect(mv::apply_grid_item_selection(
            0, 3, false, false, selected, grid_selection, selection_anchor)
            && mv::apply_grid_item_selection(
                2, 3, true, false, selected, grid_selection, selection_anchor)
            && selected == std::vector<bool>({true, true, true})
            && grid_selection == 2,
        "Shift range must select the inclusive anchor-to-focus interval");

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

    using mv::GridRebuildReason;
    expect(mv::classify_grid_rebuild_reason(false, false, false, false)
            == GridRebuildReason::None,
        "an unchanged layout should not rebuild");
    expect(mv::classify_grid_rebuild_reason(true, false, false, true)
            == GridRebuildReason::Structural,
        "a user structural reflow should take precedence over dimensions arriving");
    expect(mv::classify_grid_rebuild_reason(false, true, false, false)
            == GridRebuildReason::Structural,
        "a resize or panel/full-screen width change should be structural");
    expect(mv::classify_grid_rebuild_reason(false, false, true, false)
            == GridRebuildReason::Structural,
        "a first layout or changed index size should be structural");
    expect(mv::classify_grid_rebuild_reason(false, false, false, true)
            == GridRebuildReason::BackgroundDimensions,
        "dimension generation alone should be a background rebuild");

    int background_scroll = mv::reconcile_grid_scroll_after_rebuild(
        GridRebuildReason::BackgroundDimensions, 900, true,
        100, 220, 2000, 500);
    expect(background_scroll == 900,
        "a background dimension rebuild must not jump to an off-screen selection");
    background_scroll = mv::reconcile_grid_scroll_after_rebuild(
        GridRebuildReason::BackgroundDimensions, background_scroll, true,
        1300, 1420, 2100, 500);
    expect(background_scroll == 900,
        "consecutive background dimension rebuilds must preserve user scroll");
    expect(mv::reconcile_grid_scroll_after_rebuild(
            GridRebuildReason::BackgroundDimensions, 1900, true,
            100, 220, 2000, 500) == 1500,
        "a background rebuild should still clamp scroll to the new content height");
    expect(mv::reconcile_grid_scroll_after_rebuild(
            GridRebuildReason::Structural, 900, true,
            100, 220, 2000, 500) == 100,
        "a structural rebuild should reveal a selected row above the viewport");
    expect(mv::reconcile_grid_scroll_after_rebuild(
            GridRebuildReason::Structural, 0, true,
            100, 800, 1200, 500) == 300,
        "a row taller than the viewport should keep the established bottom-alignment rule");
    expect(mv::reconcile_grid_scroll_after_rebuild(
            GridRebuildReason::Structural, 900, false,
            0, 0, 2000, 500) == 900,
        "a first layout without selection should preserve a valid scroll position");
    expect(mv::reconcile_grid_scroll_after_rebuild(
            GridRebuildReason::Structural, 100, true,
            680, 820, 1400, 500) == 320,
        "square and justified structural reflows should retain selection visibility");

    std::vector<std::wstring> indexed_paths = {
        L"A-valid.png", L"B-damaged.png"};
    std::wstring current_path = indexed_paths.front();
    int current_index = 0;
    bool has_image = true;
    expect(mv::can_delete_current_image(has_image, current_path),
        "a successfully committed image should be deletable");

    const std::wstring deleted_path = current_path;
    const int deleted_index = current_index;
    indexed_paths.erase(indexed_paths.begin());
    int open_attempts = 0;
    const auto failed_transition = mv::run_post_delete_transition(
        deleted_path, deleted_index, static_cast<int>(indexed_paths.size()),
        [&indexed_paths](int index) { return indexed_paths[static_cast<size_t>(index)]; },
        [&open_attempts](const std::wstring&, int) {
            ++open_attempts;
            return false;
        },
        current_path, current_index, has_image);
    expect(failed_transition.deleted_path == L"A-valid.png"
            && failed_transition.deleted_index == 0,
        "the production transition should stay bound to deleted A's identity");
    expect(failed_transition.successor_attempted && open_attempts == 1,
        "deleting A should attempt to open exactly one successor");
    expect(failed_transition.successor_path == L"B-damaged.png"
            && failed_transition.successor_index == 0,
        "the failed successor attempt should remain bound to indexed B");
    expect(!failed_transition.successor_opened,
        "the injected damaged B open should be reported as failed");
    expect(current_path.empty() && current_index == -1 && !has_image,
        "a damaged successor must leave current image identity cleared");
    if (mv::can_delete_current_image(has_image, current_path))
        indexed_paths.erase(indexed_paths.begin());
    expect(indexed_paths.size() == 1 && indexed_paths.front() == L"B-damaged.png",
        "a consecutive delete must leave damaged B indexed");

    current_path = L"A-valid.png";
    current_index = 0;
    has_image = true;
    const auto successful_transition = mv::run_post_delete_transition(
        L"A-valid.png", 0, 1,
        [](int) { return std::wstring(L"B-valid.png"); },
        [](const std::wstring& path, int index) {
            return path == L"B-valid.png" && index == 0;
        },
        current_path, current_index, has_image);
    expect(successful_transition.successor_opened
            && current_path == L"B-valid.png"
            && current_index == 0 && has_image,
        "a successful successor open should atomically commit B's identity");

    current_path = L"only.png";
    current_index = 0;
    has_image = true;
    bool unexpected_open = false;
    const auto final_transition = mv::run_post_delete_transition(
        L"only.png", 0, 0,
        [](int) { return std::wstring(); },
        [&unexpected_open](const std::wstring&, int) {
            unexpected_open = true;
            return true;
        },
        current_path, current_index, has_image);
    expect(!final_transition.successor_attempted && !unexpected_open,
        "deleting the final item should not attempt a successor open");
    expect(current_path.empty() && current_index == -1 && !has_image,
        "deleting the final item should clear current identity");

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }

    std::cout << "app state tests passed\n";
    return 0;
}
