#pragma once

#include <algorithm>
#include <string>

namespace mv {

enum class GridRebuildReason {
    None,
    Structural,
    BackgroundDimensions,
};

inline GridRebuildReason classify_grid_rebuild_reason(
    bool layout_dirty, bool width_changed, bool item_count_changed,
    bool dimension_generation_changed) {
    if (layout_dirty || width_changed || item_count_changed)
        return GridRebuildReason::Structural;
    if (dimension_generation_changed)
        return GridRebuildReason::BackgroundDimensions;
    return GridRebuildReason::None;
}

inline bool is_image_zoomed(float scale, float fit_scale) {
    return fit_scale > 0.0f && scale > fit_scale * 1.02f;
}

inline bool should_preserve_selection_for_drag(
    bool clicked_selected, bool shift, bool ctrl) {
    return clicked_selected && !shift && !ctrl;
}

inline int clamp_grid_scroll_position(int scroll, int total_height, int visible_height) {
    const int max_scroll = std::max(0, total_height - std::max(0, visible_height));
    return std::clamp(scroll, 0, max_scroll);
}

inline int ensure_grid_row_visible(
    int scroll, int row_top, int row_bottom, int total_height, int visible_height) {
    int adjusted = clamp_grid_scroll_position(scroll, total_height, visible_height);
    const int viewport_height = std::max(0, visible_height);
    if (row_top < adjusted) {
        adjusted = row_top;
    } else if (row_bottom > adjusted + viewport_height) {
        adjusted = row_bottom - viewport_height;
    }
    return clamp_grid_scroll_position(adjusted, total_height, visible_height);
}

inline int reconcile_grid_scroll_after_rebuild(
    GridRebuildReason reason, int scroll, bool has_selected_row,
    int row_top, int row_bottom, int total_height, int visible_height) {
    if (reason == GridRebuildReason::Structural && has_selected_row) {
        return ensure_grid_row_visible(
            scroll, row_top, row_bottom, total_height, visible_height);
    }
    return clamp_grid_scroll_position(scroll, total_height, visible_height);
}

inline bool can_delete_current_image(bool has_image, const std::wstring& current_path) {
    return has_image && !current_path.empty();
}

inline void commit_current_image_identity(
    const std::wstring& path, int index,
    std::wstring& current_path, int& current_index, bool& has_image) {
    current_path = path;
    current_index = index;
    has_image = true;
}

inline int begin_post_delete_transition(
    int removed_index, int remaining_count,
    std::wstring& current_path, int& current_index, bool& has_image) {
    current_path.clear();
    current_index = -1;
    has_image = false;
    if (removed_index < 0 || remaining_count <= 0) return -1;
    return std::min(removed_index, remaining_count - 1);
}

} // namespace mv
