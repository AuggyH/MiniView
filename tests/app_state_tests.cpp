#include "app_state.h"

#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
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

bool nearly_equal(float left, float right, float tolerance = 0.01f) {
    return std::abs(left - right) <= tolerance;
}

void test_native_owner_menu_state() {
    HMENU menu = CreatePopupMenu();
    expect(menu != nullptr, "native owner-menu test must create a popup menu");
    if (!menu) return;

    constexpr UINT disabled_id = 7001;
    constexpr UINT enabled_id = 7002;
    auto insert_owner_item = [&](UINT id, bool disabled) {
        MENUITEMINFOW item = {sizeof(item)};
        item.fMask = MIIM_FTYPE | MIIM_ID | MIIM_STATE;
        item.fType = MFT_OWNERDRAW;
        item.fState = disabled ? MFS_DISABLED : MFS_ENABLED;
        item.wID = id;
        return InsertMenuItemW(
            menu, GetMenuItemCount(menu), TRUE, &item) != FALSE;
    };

    expect(insert_owner_item(disabled_id, true),
        "native menu must accept a disabled owner-draw item");
    expect(insert_owner_item(enabled_id, false),
        "native menu must accept an enabled owner-draw item");

    MENUITEMINFOW disabled_state = {sizeof(disabled_state)};
    disabled_state.fMask = MIIM_STATE;
    expect(GetMenuItemInfoW(menu, disabled_id, FALSE, &disabled_state) != FALSE
            && (disabled_state.fState & MFS_DISABLED) == MFS_DISABLED,
        "disabled owner-draw item must retain MFS_DISABLED in native state");
    const UINT disabled_flags = GetMenuState(menu, disabled_id, MF_BYCOMMAND);
    expect(disabled_flags != static_cast<UINT>(-1)
            && (disabled_flags & (MF_DISABLED | MF_GRAYED))
                == (MF_DISABLED | MF_GRAYED),
        "native menu behavior must report the owner-draw item as unavailable");

    MENUITEMINFOW enabled_state = {sizeof(enabled_state)};
    enabled_state.fMask = MIIM_STATE;
    expect(GetMenuItemInfoW(menu, enabled_id, FALSE, &enabled_state) != FALSE
            && (enabled_state.fState & MFS_DISABLED) == 0,
        "enabled owner-draw item must remain selectable in native state");
    const UINT enabled_flags = GetMenuState(menu, enabled_id, MF_BYCOMMAND);
    expect(enabled_flags != static_cast<UINT>(-1)
            && (enabled_flags & (MF_DISABLED | MF_GRAYED)) == 0,
        "native menu behavior must report the enabled owner-draw item as available");

    DestroyMenu(menu);
}

} // namespace

int main() {
    test_native_owner_menu_state();
    using mv::RecursiveScanAction;
    expect(!mv::can_toggle_recursive(false, false, L"")
            && mv::can_toggle_recursive(false, false, L"C:\\空根目录")
            && mv::can_toggle_recursive(true, false, L"C:\\空根目录")
            && !mv::can_toggle_recursive(false, true, L"C:\\图片目录"),
        "recursive commands must be available for a bound empty root and grid only");
    expect(mv::classify_recursive_scan_action(false, false, 2)
            == RecursiveScanAction::EnterUnselectedGrid,
        "recursive descendants from an empty root must enter an unselected grid");
    expect(mv::classify_recursive_scan_action(false, false, 0)
            == RecursiveScanAction::KeepView,
        "an empty recursive result must retain the explicit empty-root view");
    expect(mv::classify_recursive_scan_action(true, true, 0)
            == RecursiveScanAction::ShowEmptyRoot,
        "turning recursion off with no direct images must leave the blank grid");

    mv::GridEntryRouteState entry_state;
    entry_state.grid_mode = true;
    entry_state.selected_index = 1;
    entry_state.hit_index = 1;
    entry_state.item_count = 3;
    const auto space_entry = mv::route_grid_entry(
        mv::GridEntryTrigger::Space, entry_state);
    const auto double_click_entry = mv::route_grid_entry(
        mv::GridEntryTrigger::DoubleClick, entry_state);
    expect(space_entry && double_click_entry
            && space_entry->index == 1 && double_click_entry->index == 1,
        "Space and thumbnail double-click must route to the same grid item");

    entry_state.selected_index = -1;
    entry_state.hit_index = 2;
    const auto no_selection_double_click = mv::route_grid_entry(
        mv::GridEntryTrigger::DoubleClick, entry_state);
    expect(!mv::route_grid_entry(mv::GridEntryTrigger::Space, entry_state),
        "Space without a valid grid selection must fail closed");
    expect(no_selection_double_click && no_selection_double_click->index == 2,
        "double-click must bind its hit when recursive grid has no default selection");

    entry_state.selected_index = 0;
    entry_state.hit_index = 2;
    const auto different_space_entry = mv::route_grid_entry(
        mv::GridEntryTrigger::Space, entry_state);
    const auto different_double_click_entry = mv::route_grid_entry(
        mv::GridEntryTrigger::DoubleClick, entry_state);
    expect(different_space_entry && different_space_entry->index == 0
            && different_double_click_entry
            && different_double_click_entry->index == 2,
        "each trigger must bind its exact request index when hit differs from selection");

    entry_state.hit_index = -1;
    expect(!mv::route_grid_entry(
            mv::GridEntryTrigger::DoubleClick, entry_state),
        "double-click outside a thumbnail must fail closed");
    entry_state.hit_index = 1;
    entry_state.animating = true;
    expect(!mv::route_grid_entry(mv::GridEntryTrigger::Space, entry_state)
            && !mv::route_grid_entry(
                mv::GridEntryTrigger::DoubleClick, entry_state),
        "mode-switch inputs during the transition must remain ignored");

    mv::GridExitRouteState exit_state;
    exit_state.from_grid = true;
    exit_state.has_image = true;
    expect(mv::route_grid_exit(mv::GridExitTrigger::Space, exit_state)
            && mv::route_grid_exit(mv::GridExitTrigger::Escape, exit_state)
            && mv::route_grid_exit(mv::GridExitTrigger::DoubleClick, exit_state),
        "Space, Escape, and big-image double-click must route back to grid");
    exit_state.from_grid = false;
    expect(!mv::route_grid_exit(mv::GridExitTrigger::Space, exit_state)
            && !mv::route_grid_exit(mv::GridExitTrigger::Escape, exit_state)
            && mv::route_grid_exit(mv::GridExitTrigger::DoubleClick, exit_state),
        "only big-image double-click may return without saved grid context");
    exit_state.animating = true;
    expect(!mv::route_grid_exit(mv::GridExitTrigger::Space, exit_state)
            && !mv::route_grid_exit(mv::GridExitTrigger::Escape, exit_state)
            && !mv::route_grid_exit(mv::GridExitTrigger::DoubleClick, exit_state),
        "all mode-switch inputs must remain ignored during animation");

    mv::GridScrollPause scroll_pause;
    std::vector<std::uintptr_t> cancelled_timers;
    const auto cancel_timer = [&cancelled_timers](std::uintptr_t timer) {
        cancelled_timers.push_back(timer);
    };
    scroll_pause.begin(cancel_timer, [] { return std::uintptr_t{17}; });
    expect(scroll_pause.active() && scroll_pause.timer() == 17,
        "a created scroll timer must pause visible thumbnail requests");
    std::vector<int> requested_thumbnails;
    scroll_pause.request_visible(
        true, 40, 44, [&requested_thumbnails](int index) {
            requested_thumbnails.push_back(index);
        });
    expect(requested_thumbnails.empty(),
        "active scrolling must continue suppressing visible thumbnail requests");

    scroll_pause.begin(cancel_timer, [] { return std::uintptr_t{0}; });
    expect(!scroll_pause.active() && scroll_pause.timer() == 0
            && cancelled_timers == std::vector<std::uintptr_t>{17},
        "SetTimer failure must cancel the prior timer and immediately resume scheduling");
    scroll_pause.request_visible(
        true, 40, 44, [&requested_thumbnails](int index) {
            requested_thumbnails.push_back(index);
        });
    expect(requested_thumbnails == std::vector<int>({40, 41, 42, 43}),
        "SetTimer failure must make the exact visible range requestable");

    requested_thumbnails.clear();
    scroll_pause.begin(cancel_timer, [] { return std::uintptr_t{23}; });
    scroll_pause.finish(cancel_timer);
    expect(!scroll_pause.active() && scroll_pause.timer() == 0
            && cancelled_timers == std::vector<std::uintptr_t>({17, 23}),
        "grid exit must cancel its timer and end the scroll pause");
    scroll_pause.request_visible(
        false, 50, 53, [&requested_thumbnails](int index) {
            requested_thumbnails.push_back(index);
        });
    expect(requested_thumbnails.empty(),
        "a stopped loader must not accumulate visible requests");
    scroll_pause.request_visible(
        true, 50, 53, [&requested_thumbnails](int index) {
            requested_thumbnails.push_back(index);
        });
    expect(requested_thumbnails == std::vector<int>({50, 51, 52}),
        "a restarted loader must receive the exact visible range after grid exit");

    mv::GridTransitionGeometry geometry;
    geometry.request_index = 2;
    geometry.item_count = 3;
    geometry.row_start_index = 1;
    geometry.row_end_index = 3;
    geometry.row_y = 600;
    geometry.row_height = 200;
    geometry.item_x = 300.0f;
    geometry.item_width = 150.0f;
    geometry.image_width = 300;
    geometry.image_height = 200;
    geometry.thumb_padding = 8;
    geometry.toolbar_height = 56;
    geometry.scroll_y = 400;
    const auto source_rect = mv::calculate_grid_transition_rect(geometry);
    expect(source_rect
            && nearly_equal((source_rect->left + source_rect->right) * 0.5f, 383.0f)
            && nearly_equal((source_rect->top + source_rect->bottom) * 0.5f, 356.0f)
            && nearly_equal((source_rect->right - source_rect->left)
                / (source_rect->bottom - source_rect->top), 1.5f),
        "transition source must use the exact item geometry and scrolled viewport coordinates");
    geometry.scroll_y = 450;
    const auto scrolled_source_rect = mv::calculate_grid_transition_rect(geometry);
    expect(source_rect && scrolled_source_rect
            && nearly_equal(scrolled_source_rect->top, source_rect->top - 50.0f)
            && nearly_equal(scrolled_source_rect->bottom, source_rect->bottom - 50.0f),
        "transition source must move by the exact grid scroll delta");
    geometry.request_index = 0;
    expect(!mv::calculate_grid_transition_rect(geometry),
        "an index outside the captured layout row must not reuse an old source rect");
    geometry.request_index = 2;
    geometry.item_width = 0.0f;
    expect(!mv::calculate_grid_transition_rect(geometry),
        "invalid item geometry must fail closed instead of returning a stale rect");

    auto expect_successful_entry = [&](
            const mv::GridEntryRequest& request, bool capture_throws) {
        bool grid_mode = true;
        bool from_grid = false;
        bool animation_started = false;
        std::vector<std::string> stages;
        const bool entered = mv::run_grid_entry(
            request,
            mv::GridEntryTransactionState{
                grid_mode, from_grid, animation_started},
            [&](int index) {
                expect(index == 1, "entry transition must bind the requested index");
                stages.push_back("capture");
                if (capture_throws)
                    throw std::runtime_error("injected transition capture failure");
            },
            [&](int index) {
                expect(index == 1, "image load must bind the requested index");
                const bool loaded = mv::run_image_load_stages(
                    [&]() {
                        stages.push_back("decode");
                        return 1;
                    },
                    [&](int decoded) {
                        stages.push_back("materialize");
                        return decoded + 1;
                    },
                    [&](int materialized) {
                        stages.push_back("upload");
                        return materialized == 2;
                    });
                if (loaded) {
                    stages.push_back("commit");
                    grid_mode = false;
                    from_grid = true;
                }
                return loaded;
            },
            [&]() {
                stages.push_back("animation");
                animation_started = true;
            });
        expect(entered && !grid_mode && from_grid && animation_started,
            "a successful load must commit big-image mode");
        expect(stages == std::vector<std::string>(
                {"capture", "decode", "materialize", "upload", "commit", "animation"}),
            capture_throws
                ? "capture failure must not block formal loading and entry"
                : "grid entry must commit before the visual animation begins");
    };
    expect_successful_entry(*space_entry, false);
    expect_successful_entry(*double_click_entry, false);
    expect_successful_entry(*space_entry, true);

    enum class LoadFault { Decode, Materialize, Upload };
    auto expect_failed_load = [&](LoadFault fault) {
        bool grid_mode = true;
        bool from_grid = false;
        bool animation_started = false;
        int decode_calls = 0;
        int materialize_calls = 0;
        int upload_calls = 0;
        const bool entered = mv::run_grid_entry(
            *space_entry,
            mv::GridEntryTransactionState{
                grid_mode, from_grid, animation_started},
            [](int) {},
            [&](int) {
                const bool loaded = mv::run_image_load_stages(
                    [&]() {
                        ++decode_calls;
                        if (fault == LoadFault::Decode)
                            throw std::runtime_error("injected decode failure");
                        return 1;
                    },
                    [&](int decoded) {
                        ++materialize_calls;
                        if (fault == LoadFault::Materialize)
                            throw std::runtime_error("injected materialize failure");
                        return decoded + 1;
                    },
                    [&](int) {
                        ++upload_calls;
                        return fault != LoadFault::Upload;
                    });
                if (loaded) {
                    grid_mode = false;
                    from_grid = true;
                }
                return loaded;
            },
            [&]() { animation_started = true; });
        expect(!entered && grid_mode && !from_grid && !animation_started,
            "each formal load-stage failure must preserve real grid transaction state");
        expect(decode_calls == 1
                && materialize_calls == (fault == LoadFault::Decode ? 0 : 1)
                && upload_calls == (fault == LoadFault::Upload ? 1 : 0),
            "fault injection must stop at the exact failing production load stage");
    };
    expect_failed_load(LoadFault::Decode);
    expect_failed_load(LoadFault::Materialize);
    expect_failed_load(LoadFault::Upload);

    bool restored_grid_mode = true;
    bool restored_from_grid = false;
    bool restored_animation = false;
    const bool partial_commit = mv::run_grid_entry(
        *space_entry,
        mv::GridEntryTransactionState{
            restored_grid_mode, restored_from_grid, restored_animation},
        [](int) {},
        [&](int) {
            restored_grid_mode = false;
            restored_from_grid = true;
            return false;
        },
        [&]() { restored_animation = true; });
    expect(!partial_commit && restored_grid_mode && !restored_from_grid
            && !restored_animation,
        "a failed production transaction must restore partially changed mode state");

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

    bool show_labels = true;
    bool label_layout_dirty = false;
    expect(mv::apply_grid_label_toggle(
            true, show_labels, label_layout_dirty)
            && !show_labels && label_layout_dirty,
        "grid label toggle must hide labels and request a structural rebuild");
    label_layout_dirty = false;
    expect(mv::apply_grid_label_toggle(
            true, show_labels, label_layout_dirty)
            && show_labels && label_layout_dirty,
        "a second grid label toggle must restore labels and rebuild layout");
    label_layout_dirty = false;
    expect(!mv::apply_grid_label_toggle(
            false, show_labels, label_layout_dirty)
            && show_labels && !label_layout_dirty,
        "big-image mode must ignore the label toggle without changing layout state");

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
