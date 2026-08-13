#include "navstate.h"
#include "indexer.h"
#include "renderer_state.h"

#include <algorithm>
#include <cmath>
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

void expect_eq(int left, int right, const char* message) {
    if (left != right) {
        std::cerr << "FAIL: " << message << " (expected " << right
                  << ", got " << left << ")\n";
        ++failures;
    }
}

void expect_eq_uint(std::uint64_t left, std::uint64_t right,
                    const char* message) {
    if (left != right) {
        std::cerr << "FAIL: " << message << " (expected " << right
                  << ", got " << left << ")\n";
        ++failures;
    }
}

void expect_eq_str(const std::wstring& left, const std::wstring& right,
                   const char* message) {
    if (left != right) {
        std::wcerr << L"FAIL: " << message << L" (expected '" << right
                   << L"', got '" << left << L"')\n";
        ++failures;
    }
}

// ── NavPanelState: expand state machine ─────────────────────

void test_panel_state() {
    mv::NavPanelState state;
    // Grid mode shows by default, image mode hides by default.
    expect(state.visible(true), "panel default visible in grid");
    expect(!state.visible(false), "panel default hidden in image mode");
    expect(state.auto_managed(), "panel auto-managed before first toggle");
    expect(state.expanded(), "panel expanded flag default true");

    // First toggle fixes the flag; retained across mode switches.
    state.toggle();
    expect(!state.auto_managed(), "panel no longer auto-managed");
    expect(!state.expanded(), "panel collapsed after toggle");
    expect(!state.visible(true), "panel hidden in grid after toggle");
    expect(!state.visible(false), "panel hidden in image mode after toggle");

    state.toggle();
    expect(state.expanded(), "panel expanded after second toggle");
    expect(state.visible(true), "panel visible in grid");
    expect(state.visible(false), "panel visible in image mode (retained)");

    // Focus cycle: Main → LeftPanel → Main.
    expect(!state.focused(), "focus starts in main");
    expect(state.cycle_focus(), "tab moves focus into panel");
    expect(state.focused(), "focus in panel after tab");
    expect(!state.cycle_focus(), "tab moves focus back to main");
    expect(!state.focused(), "focus back to main");

    state.set_focus(mv::NavFocusTarget::LeftPanel);
    expect(state.focused(), "explicit panel focus");
    state.release_focus();
    expect(!state.focused(), "focus released");

    // Toggling the panel closed drops the focus.
    state.set_focus(mv::NavFocusTarget::LeftPanel);
    state.toggle();  // expanded(false)
    expect(!state.focused(), "closing panel releases focus");
    state.toggle();  // back to expanded

    // Tab state.
    expect(state.tab() == mv::NavPanelTab::Directories,
           "default tab is directories");
    state.set_tab(mv::NavPanelTab::Favorites);
    expect(state.tab() == mv::NavPanelTab::Favorites, "tab switched");
    state.toggle_tab();
    expect(state.tab() == mv::NavPanelTab::Directories, "tab toggled back");
}

// ── NavSwitchController: generation cancellation ────────────

void test_switch_controller() {
    mv::NavSwitchController controller;
    expect(!controller.in_flight(), "no switch in flight initially");
    expect(!controller.finish(0), "finish without request rejected");

    const std::uint64_t first = controller.request();
    expect_eq_uint(first, 1, "first request generation");
    expect(controller.in_flight(), "in flight after request");
    expect(!controller.finish(first + 1), "future generation rejected");
    expect(controller.finish(first), "current generation applies");

    // A new request invalidates the previous one.
    const std::uint64_t a = controller.request();
    const std::uint64_t b = controller.request();
    expect(b > a, "second request bumps generation");
    expect(!controller.finish(a), "superseded request rejected");
    expect(controller.finish(b), "latest request applies");

    // invalidate() cancels without a new scan.
    const std::uint64_t c = controller.request();
    controller.invalidate();
    expect(!controller.in_flight(), "invalidate clears in-flight");
    expect(!controller.finish(c), "finish after invalidate rejected");
}

// ── CollectionSortMemory: per-collection state ──────────────

void test_collection_memory() {
    mv::CollectionSortMemory memory;
    expect(memory.size() == 0, "memory starts empty");
    const mv::CollectionMemory fresh = memory.memory_for(L"D:\\AIGC");
    expect(fresh.sort == mv::SortMode::Name, "default sort is name");
    expect(!fresh.recursive, "default non-recursive");

    memory.remember(L"D:\\AIGC", {mv::SortMode::Date, true});
    expect(memory.size() == 1, "one collection remembered");
    const mv::CollectionMemory restored =
        memory.memory_for(L"d:/aigc");  // case/separator normalized
    expect(restored.sort == mv::SortMode::Date, "sort restored");
    expect(restored.recursive, "recursive restored");

    // Collections are independent.
    const mv::CollectionMemory other = memory.memory_for(L"D:\\Other");
    expect(other.sort == mv::SortMode::Name, "other collection keeps default");

    memory.forget(L"D:\\AIGC");
    expect(memory.memory_for(L"D:\\AIGC").sort == mv::SortMode::Name,
           "forgotten collection falls back to default");
}

// ── Path segments / breadcrumb ──────────────────────────────

void test_path_segments() {
    using mv::split_path_segments;

    const auto nested = split_path_segments(L"D:\\AIGC\\ComfyUI\\output");
    expect_eq(static_cast<int>(nested.size()), 4, "nested segment count");
    expect_eq_str(nested[0], L"D:", "drive root segment");
    expect_eq_str(nested[1], L"AIGC", "first component");
    expect_eq_str(nested[3], L"output", "last component");

    const auto root_only = split_path_segments(L"C:\\");
    expect_eq(static_cast<int>(root_only.size()), 1, "root segment count");
    expect_eq_str(root_only[0], L"C:", "drive-only path");

    const auto bare_drive = split_path_segments(L"D:");
    expect_eq(static_cast<int>(bare_drive.size()), 1, "bare drive count");
    expect_eq_str(bare_drive[0], L"D:", "bare drive segment");

    const auto trailing = split_path_segments(L"D:\\AIGC\\");
    expect_eq(static_cast<int>(trailing.size()), 2, "trailing slash count");
    expect_eq_str(trailing[1], L"AIGC", "trailing slash stripped");

    const auto forward = split_path_segments(L"D:/a/b/c");
    expect_eq(static_cast<int>(forward.size()), 4, "forward slash count");
    expect_eq_str(forward[2], L"b", "forward slash component");

    const auto empty = split_path_segments(L"");
    expect(empty.empty(), "empty path has no segments");
}

void test_path_reconstruction() {
    using mv::path_from_segments;
    const std::vector<std::wstring> segments = {
        L"D:", L"AIGC", L"ComfyUI", L"output"};

    expect_eq_str(path_from_segments(segments, 0), L"D:", "drive only");
    expect_eq_str(path_from_segments(segments, 1), L"D:\\AIGC",
                  "one level");
    expect_eq_str(path_from_segments(segments, 3), L"D:\\AIGC\\ComfyUI\\output",
                  "full path");
    expect_eq_str(path_from_segments(segments, -1), L"", "negative index");
    expect_eq_str(path_from_segments(segments, 99), L"D:\\AIGC\\ComfyUI\\output",
                  "index clamped");
}

// Fake text measure: 10 units per character.
float fake_measure(const std::wstring& text) {
    return static_cast<float>(text.size()) * 10.0f;
}

void test_breadcrumb_layout() {
    using mv::NavBreadcrumbLayout;
    using mv::layout_breadcrumb;

    const std::vector<std::wstring> segments = {
        L"D:", L"AIGC", L"ComfyUI", L"output"};

    // Everything fits → all segments, no truncation.
    const NavBreadcrumbLayout fits = layout_breadcrumb(
        segments, 500.0f, 4.0f, 12.0f, fake_measure);
    expect(!fits.truncated, "fits: not truncated");
    expect_eq(static_cast<int>(fits.items.size()), 4, "fits: all items");
    expect_eq(fits.items[0].segment_index, 0, "fits: first segment");
    expect_eq(fits.items[3].segment_index, 3, "fits: last segment");
    expect(!fits.items[1].ellipsis, "fits: no ellipsis");
    // x positions advance by width + gap.
    expect(std::abs(fits.items[1].x - (fits.items[0].width + 4.0f)) < 0.01f,
           "fits: x advances");

    // Overflow → first + ellipsis + last.
    const NavBreadcrumbLayout truncated = layout_breadcrumb(
        segments, 140.0f, 4.0f, 12.0f, fake_measure);
    expect(truncated.truncated, "truncated flag");
    expect_eq(static_cast<int>(truncated.items.size()), 3, "truncated: 3 slots");
    expect_eq(truncated.items[0].segment_index, 0, "truncated: first kept");
    expect(truncated.items[1].ellipsis, "truncated: ellipsis kept");
    expect_eq(truncated.items[2].segment_index, 3, "truncated: last kept");

    // Severe overflow → ellipsis dropped, first + last only.
    const NavBreadcrumbLayout severe = layout_breadcrumb(
        segments, 90.0f, 4.0f, 12.0f, fake_measure);
    expect(severe.truncated, "severe: truncated");
    expect_eq(static_cast<int>(severe.items.size()), 2, "severe: 2 slots");
    expect(!severe.items[0].ellipsis && !severe.items[1].ellipsis,
           "severe: no ellipsis");

    // Worst → only the shortest survivor fits (drive root here).
    const NavBreadcrumbLayout worst = layout_breadcrumb(
        segments, 45.0f, 4.0f, 12.0f, fake_measure);
    expect_eq(static_cast<int>(worst.items.size()), 1, "worst: 1 slot");
    expect_eq(worst.items[0].segment_index, 0, "worst: shortest kept");

    // Single segment never truncates.
    const std::vector<std::wstring> single = {L"D:"};
    const NavBreadcrumbLayout one = layout_breadcrumb(
        single, 10.0f, 4.0f, 12.0f, fake_measure);
    expect_eq(static_cast<int>(one.items.size()), 1, "single: one item");
    expect(!one.truncated, "single: not truncated");
}

void test_breadcrumb_hit() {
    using mv::breadcrumb_hit_item;
    using mv::breadcrumb_hit_segment;
    using mv::layout_breadcrumb;

    const std::vector<std::wstring> segments = {
        L"D:", L"AIGC", L"ComfyUI", L"output"};
    const auto layout =
        layout_breadcrumb(segments, 500.0f, 4.0f, 12.0f, fake_measure);
    // widths: 20, 40, 70, 60; x: 0, 24, 68, 142
    expect_eq(breadcrumb_hit_segment(layout, 5.0f), 0, "hit first");
    expect_eq(breadcrumb_hit_segment(layout, 30.0f), 1, "hit second");
    expect_eq(breadcrumb_hit_segment(layout, 150.0f), 3, "hit last");
    expect_eq(breadcrumb_hit_segment(layout, 10.0f), 0, "hit within first");
    expect_eq(breadcrumb_hit_segment(layout, 22.0f), -1, "gap not hit");
    expect_eq(breadcrumb_hit_segment(layout, 300.0f), -1, "past end not hit");

    // Item hit returns the layout slot index.
    expect_eq(breadcrumb_hit_item(layout, 5.0f), 0, "item hit 0");
    expect_eq(breadcrumb_hit_item(layout, 30.0f), 1, "item hit 1");
    expect_eq(breadcrumb_hit_item(layout, 300.0f), -1, "item miss");

    // Truncated layout: first + ellipsis + last; ellipsis slot never hits.
    // x: 0(20) | 24(12 ellipsis) | 40(60 tail) → tail spans [40, 100).
    const auto trunc =
        layout_breadcrumb(segments, 140.0f, 4.0f, 12.0f, fake_measure);
    expect_eq(breadcrumb_hit_segment(trunc, 25.0f), -1, "ellipsis not hit");
    expect_eq(breadcrumb_hit_segment(trunc, 5.0f), 0, "truncated first hit");
    expect_eq(breadcrumb_hit_segment(trunc, 60.0f), 3, "truncated last hit");
}

// ── NavTreeModel: lazy expand / count / reveal ──────────────

void test_tree_model() {
    mv::NavTreeModel tree;

    // Roots.
    const std::uint64_t root = tree.add_root(L"D:\\", L"D:");
    expect(root != 0, "root created");
    expect_eq_uint(tree.add_root(L"D:\\", L"D:"), root, "root dedup");
    expect_eq_uint(tree.add_root(L"d:/", L"D:"), root, "root dedup case");

    // Lazy expand: request → generation; finish applies sorted children +
    // image count.
    const std::uint64_t gen = tree.request_expand(root);
    expect(gen != 0, "root expand gets generation");
    expect_eq_uint(tree.request_expand(root), 0, "re-request while loading");
    std::vector<mv::NavChildInfo> children = {
        {L"D:\\zeta", L"zeta"},
        {L"D:\\alpha", L"alpha"},
        {L"D:\\beta", L"beta"},
    };
    expect(tree.finish_expand(root, gen, children, 42, true, L""),
           "expand result applied");
    const mv::NavTreeNode* node = tree.node(root);
    expect(node != nullptr, "root node exists");
    expect(node->state == mv::NavNodeState::Expanded, "root expanded");
    expect_eq(node->image_count, 42, "image count cached");
    expect_eq(static_cast<int>(node->children.size()), 3, "children count");
    const auto& first_child = tree.node(node->children[0]);
    expect(first_child != nullptr && first_child->name == L"alpha",
           "children sorted by name");

    // Stale generation rejected.
    expect(!tree.finish_expand(root, gen + 99, {}, 0, true, L""),
           "stale generation rejected");
    // Not-loading rejected.
    expect(!tree.finish_expand(root, gen, {}, 0, true, L""),
           "finish when not loading rejected");

    // Expand a child and apply an error.
    const std::uint64_t child_gen =
        tree.request_expand(node->children[0]);
    expect(child_gen != 0, "child expand requested");
    expect(tree.finish_expand(node->children[0], child_gen, {}, 0, false,
                              L"\u65E0\u6CD5\u8BFB\u53D6"),
           "error result applied");
    const mv::NavTreeNode* alpha = tree.node(node->children[0]);
    expect(alpha->state == mv::NavNodeState::Error, "error state");
    expect(!alpha->error.empty(), "error message stored");

    // Error node can be retried (new generation).
    const std::uint64_t retry_gen =
        tree.request_expand(node->children[0]);
    expect(retry_gen != 0, "error node retry gets generation");
    expect(tree.finish_expand(node->children[0], retry_gen,
                              {{L"D:\\alpha\\deep", L"deep"}}, 7, true, L""),
           "retry applied");
    expect(tree.node(node->children[0])->state == mv::NavNodeState::Expanded,
           "retry expanded");

    // Collapse keeps the cache; re-expand is instant (no generation).
    tree.collapse(root);
    expect(tree.node(root)->state == mv::NavNodeState::Collapsed,
           "collapsed");
    expect_eq_uint(tree.request_expand(root), 0, "cached re-expand");
    expect(tree.node(root)->state == mv::NavNodeState::Expanded,
           "cached re-expand instant");

    // Cancelled finish drops the node back to Collapsed.
    const std::uint64_t cancel_gen =
        tree.request_expand(node->children[1]);
    expect(tree.finish_expand(node->children[1], cancel_gen, {}, 0, false,
                              L"", true),
           "cancelled finish applies");
    expect(tree.node(node->children[1])->state == mv::NavNodeState::Collapsed,
           "cancelled node collapsed");
}

void test_tree_reveal() {
    mv::NavTreeModel tree;
    const std::uint64_t root = tree.add_root(L"D:\\", L"D:");
    tree.finish_expand(root, tree.request_expand(root),
                       {{L"D:\\AIGC", L"AIGC"}, {L"D:\\Other", L"Other"}},
                       10, true, L"");
    const std::uint64_t aigc = tree.node(root)->children[0];

    // Path not under the tree → no highlight, no expansions.
    const auto outside = tree.reveal(L"E:\\something");
    expect(outside.highlight_id == 0, "outside path no highlight");
    expect(outside.expansions.empty(), "outside path no expansions");

    // Path exactly at the root.
    const auto at_root = tree.reveal(L"D:\\");
    expect_eq_uint(at_root.highlight_id, root, "root highlight");

    // Path one level deep: the node already exists (enumerated with the
    // root), so it is highlighted directly; no expansion is needed.
    const auto one_level = tree.reveal(L"D:\\AIGC");
    expect_eq_uint(one_level.highlight_id, aigc,
                   "known path node highlighted directly");
    expect(one_level.expansions.empty(), "no expansion for known node");

    // After expanding, the path node itself is the highlight.
    tree.finish_expand(aigc, tree.request_expand(aigc),
                       {{L"D:\\AIGC\\output", L"output"}}, 5, true, L"");
    const auto known = tree.reveal(L"D:\\AIGC");
    expect_eq_uint(known.highlight_id, aigc, "expanded node highlighted");

    // Deep path: chained expansion through the tree.
    const auto deep = tree.reveal(L"D:\\AIGC\\output\\sub");
    expect_eq(static_cast<int>(deep.expansions.size()), 1,
              "deep: one expansion (output)");
    expect_eq_uint(deep.highlight_id, aigc, "deep: highlight stays at AIGC");
    const std::uint64_t output = tree.node(aigc)->children[0];
    tree.finish_expand(output, tree.request_expand(output),
                       {{L"D:\\AIGC\\output\\sub", L"sub"}}, 0, true, L"");
    // After the expansion chain lands, the path node itself is highlighted
    // and no further enumeration is needed.
    const auto deep2 = tree.reveal(L"D:\\AIGC\\output\\sub");
    const std::uint64_t sub = tree.node(output)->children[0];
    expect_eq_uint(deep2.highlight_id, sub, "deep: path node highlighted");
    expect(deep2.expansions.empty(), "deep: no expansion for known node");
}

void test_tree_rows_and_focus() {
    mv::NavTreeModel tree;
    const std::uint64_t root = tree.add_root(L"D:\\", L"D:");
    tree.finish_expand(root, tree.request_expand(root),
                       {{L"D:\\a", L"a"}, {L"D:\\b", L"b"}}, 0, true, L"");
    const auto& root_node = *tree.node(root);
    const std::uint64_t a = root_node.children[0];
    const std::uint64_t b = root_node.children[1];
    tree.finish_expand(a, tree.request_expand(a),
                       {{L"D:\\a\\a1", L"a1"}}, 3, true, L"");
    const std::uint64_t a1 = tree.node(a)->children[0];

    // Total visible rows: root, a, a1, b = 4.
    expect_eq_uint(tree.visible_row_count(), 4, "visible row count");
    const float row_h = 28.0f;
    expect(std::abs(tree.content_height(row_h) - 112.0f) < 0.01f,
           "content height");

    // Culling: first two rows visible.
    const auto rows = tree.layout_rows(0.0f, 56.0f, row_h, 0.0f);
    expect_eq(static_cast<int>(rows.size()), 2, "culled row count");
    expect_eq_uint(rows[0].node_id, root, "first visible row is root");
    expect_eq_uint(rows[1].node_id, a, "second visible row is a");
    expect_eq(rows[1].depth, 1, "depth of a");
    expect(std::abs(rows[1].y - 28.0f) < 0.01f, "row y position");

    // Scrolled viewport [56, 112) shows rows touching the boundary too:
    // a ends exactly at 56, a1 starts at 56, b ends at 112.
    const auto scrolled = tree.layout_rows(56.0f, 56.0f, row_h, 0.0f);
    expect_eq(static_cast<int>(scrolled.size()), 3, "scrolled row count");
    expect_eq_uint(scrolled[0].node_id, a, "scrolled row a (touching top)");
    expect_eq_uint(scrolled[1].node_id, a1, "scrolled row a1");
    expect_eq_uint(scrolled[2].node_id, b, "scrolled row b");

    // Collapse `a` → its children disappear from the visible set.
    tree.collapse(a);
    expect_eq_uint(tree.visible_row_count(), 3, "collapsed row count");
    const auto collapsed_rows = tree.layout_rows(0.0f, 200.0f, row_h, 0.0f);
    expect_eq(static_cast<int>(collapsed_rows.size()), 3,
              "collapsed layout rows");

    // DFS focus navigation.
    tree.request_expand(a);  // cached re-expand
    expect_eq_uint(tree.focus_next(root), a, "focus next root→a");
    expect_eq_uint(tree.focus_next(a), a1, "focus next a→a1");
    expect_eq_uint(tree.focus_next(a1), b, "focus next a1→b");
    expect_eq_uint(tree.focus_next(b), b, "focus next clamps at end");
    expect_eq_uint(tree.focus_prev(b), a1, "focus prev b→a1");
    expect_eq_uint(tree.focus_prev(root), root, "focus prev clamps at start");
}

// ── Row click zones ─────────────────────────────────────────

void test_tree_row_zones() {
    using mv::hit_nav_tree_row;
    using mv::NavTreeRowZone;
    const float row_left = 8.0f;
    const float indent = 16.0f;
    const float arrow_w = 20.0f;
    // Depth 0: arrow zone [8, 28).
    expect(hit_nav_tree_row(row_left, 10.0f, 0, indent, arrow_w)
               == NavTreeRowZone::Arrow,
           "depth0 arrow");
    expect(hit_nav_tree_row(row_left, 30.0f, 0, indent, arrow_w)
               == NavTreeRowZone::Body,
           "depth0 body");
    expect(hit_nav_tree_row(row_left, 5.0f, 0, indent, arrow_w)
               == NavTreeRowZone::None,
           "before row is none");
    // Depth 2: arrow zone [40, 60).
    expect(hit_nav_tree_row(row_left, 45.0f, 2, indent, arrow_w)
               == NavTreeRowZone::Arrow,
           "depth2 arrow");
    expect(hit_nav_tree_row(row_left, 61.0f, 2, indent, arrow_w)
               == NavTreeRowZone::Body,
           "depth2 body");
}

// ── Collection switch apply planning ────────────────────────

void test_apply_planning() {
    using mv::CollectionApplyAction;
    using mv::plan_collection_apply;

    expect(plan_collection_apply({-1, false}) == CollectionApplyAction::ShowOpenError,
           "failed scan shows error");
    expect(plan_collection_apply({-1, true}) == CollectionApplyAction::ShowOpenError,
           "failed scan shows error in grid");
    expect(plan_collection_apply({0, false}) == CollectionApplyAction::EnterGrid,
           "empty scan enters grid");
    expect(plan_collection_apply({12, false}) == CollectionApplyAction::EnterGrid,
           "success enters grid from image mode");
    expect(plan_collection_apply({12, true}) == CollectionApplyAction::RefreshGrid,
           "success refreshes grid");

    // A switched collection never inherits a selection.
    int current_idx = 4, grid_sel = 4, grid_saved_idx = 4;
    std::vector<bool> selected = {false, true, false};
    int anchor = 4;
    mv::reset_collection_selection(
        current_idx, grid_sel, grid_saved_idx, selected, anchor);
    expect_eq(current_idx, -1, "current index reset");
    expect_eq(grid_sel, -1, "grid selection reset (no default selection)");
    expect_eq(grid_saved_idx, -1, "saved index reset");
    expect(selected.empty(), "selection cleared");
    expect_eq(anchor, -1, "anchor reset");
}

// ── Nav panel geometry (renderer_state.h) ───────────────────

void test_panel_geometry() {
    const mv::NavPanelGeometry g =
        mv::build_nav_panel_geometry(0.0f, 40.0f, 240.0f, 860.0f, 1.0f);
    expect(std::abs(g.breadcrumb_h - 36.0f) < 0.01f, "breadcrumb height");
    expect(std::abs(g.tabs_y - 76.0f) < 0.01f, "tabs below breadcrumb");
    expect(std::abs(g.tabs_h - 28.0f) < 0.01f, "tab height");
    expect(std::abs(g.stats_y - 876.0f) < 0.01f, "stats at bottom");
    expect(g.tree_h > 0.0f, "tree viewport positive");
    expect(std::abs(g.tree_y - 104.0f) < 0.01f, "tree below tabs");
    expect(std::abs(g.stats_y - (g.tree_y + g.tree_h)) < 0.01f,
           "tree ends at stats");
    expect(std::abs(g.scrollbar_x - 234.0f) < 0.01f, "scrollbar at right");

    // 200% DPI scales everything.
    const mv::NavPanelGeometry hi =
        mv::build_nav_panel_geometry(0.0f, 80.0f, 480.0f, 1720.0f, 2.0f);
    expect(std::abs(hi.breadcrumb_h - 72.0f) < 0.01f,
           "breadcrumb DPI scaled");
    expect(std::abs(hi.tabs_y - 152.0f) < 0.01f, "tabs DPI scaled");
}

// ── Image extension helper (indexer) ────────────────────────

void test_image_extension() {
    expect(mv::ImageIndex::is_supported_image_extension(L".png"),
           "png supported");
    expect(mv::ImageIndex::is_supported_image_extension(L".jpg"),
           "jpg supported");
    expect(mv::ImageIndex::is_supported_image_extension(L".webp"),
           "webp supported");
    expect(!mv::ImageIndex::is_supported_image_extension(L".txt"),
           "txt not supported");
    expect(!mv::ImageIndex::is_supported_image_extension(L".ico"),
           "ico not indexed (openable but not indexed)");
    expect(!mv::ImageIndex::is_supported_image_extension(L".PNG"),
           "extension compare is exact (worker lowercases first)");
}

}  // namespace

int main() {
    test_panel_state();
    test_switch_controller();
    test_collection_memory();
    test_path_segments();
    test_path_reconstruction();
    test_breadcrumb_layout();
    test_breadcrumb_hit();
    test_tree_model();
    test_tree_reveal();
    test_tree_rows_and_focus();
    test_tree_row_zones();
    test_apply_planning();
    test_panel_geometry();
    test_image_extension();

    if (failures == 0) {
        std::cout << "navstate.unit: all tests passed\n";
        return 0;
    }
    std::cerr << "navstate.unit: " << failures << " failure(s)\n";
    return 1;
}
