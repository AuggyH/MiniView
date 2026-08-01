#pragma once

#include <algorithm>
#include <string>
#include <vector>

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

inline bool apply_grid_item_selection(
    int index, int item_count, bool shift, bool ctrl,
    std::vector<bool>& selected, int& grid_selection, int& selection_anchor) {
    if (index < 0 || index >= item_count) return false;

    if (shift && selection_anchor >= 0) {
        int start = selection_anchor;
        int end = index;
        std::fill(selected.begin(), selected.end(), false);
        selection_anchor = -1;
        if (start > end) std::swap(start, end);
        for (int current = start;
             current <= end && current < static_cast<int>(selected.size());
             ++current) {
            selected[static_cast<size_t>(current)] = true;
        }
    } else if (ctrl) {
        if (index < static_cast<int>(selected.size()))
            selected[static_cast<size_t>(index)] =
                !selected[static_cast<size_t>(index)];
        selection_anchor = index;
    } else {
        std::fill(selected.begin(), selected.end(), false);
        selection_anchor = -1;
        if (index < static_cast<int>(selected.size()))
            selected[static_cast<size_t>(index)] = true;
        selection_anchor = index;
    }

    grid_selection = index;
    return true;
}

inline bool grid_item_is_selected(
    int index, const std::vector<bool>& selected) {
    return index >= 0 && index < static_cast<int>(selected.size())
        && selected[static_cast<size_t>(index)];
}

inline bool grid_item_has_selection_border(
    int index, int /*grid_selection*/, const std::vector<bool>& selected) {
    return grid_item_is_selected(index, selected);
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

struct PostDeleteTransitionResult {
    std::wstring deleted_path;
    int deleted_index = -1;
    std::wstring successor_path;
    int successor_index = -1;
    bool successor_attempted = false;
    bool successor_opened = false;
};

template <typename RemainingPathAt, typename OpenSuccessor>
inline PostDeleteTransitionResult run_post_delete_transition(
    const std::wstring& deleted_path, int deleted_index, int remaining_count,
    RemainingPathAt remaining_path_at, OpenSuccessor open_successor,
    std::wstring& current_path, int& current_index, bool& has_image) {
    PostDeleteTransitionResult result;
    result.deleted_path = deleted_path;
    result.deleted_index = deleted_index;

    current_path.clear();
    current_index = -1;
    has_image = false;
    if (deleted_index < 0 || remaining_count <= 0) return result;

    result.successor_index = std::min(deleted_index, remaining_count - 1);
    result.successor_path = remaining_path_at(result.successor_index);
    result.successor_attempted = true;
    result.successor_opened =
        open_successor(result.successor_path, result.successor_index);
    if (result.successor_opened) {
        commit_current_image_identity(
            result.successor_path, result.successor_index,
            current_path, current_index, has_image);
    } else {
        current_path.clear();
        current_index = -1;
        has_image = false;
    }
    return result;
}

} // namespace mv
