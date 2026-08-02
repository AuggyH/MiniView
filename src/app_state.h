#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mv {

enum class GridRebuildReason {
    None,
    Structural,
    BackgroundDimensions,
};

enum class GridEntryTrigger {
    Space,
    DoubleClick,
};

struct GridEntryRouteState {
    bool grid_mode = false;
    bool animating = false;
    int selected_index = -1;
    int hit_index = -1;
    int item_count = 0;
};

struct GridEntryRequest {
    GridEntryTrigger trigger = GridEntryTrigger::Space;
    int index = -1;
};

enum class GridExitTrigger {
    Space,
    Escape,
    DoubleClick,
};

struct GridExitRouteState {
    bool animating = false;
    bool from_grid = false;
    bool has_image = false;
};

struct GridEntryTransactionState {
    bool& grid_mode;
    bool& from_grid;
    bool& animating;
    int& grid_selection;
    std::vector<bool>& selected;
    int& selection_anchor;
};

struct GridTransitionGeometry {
    int request_index = -1;
    int item_count = 0;
    int row_start_index = 0;
    int row_end_index = 0;
    int row_y = 0;
    int row_height = 0;
    float item_x = 0.0f;
    float item_width = 0.0f;
    uint32_t image_width = 0;
    uint32_t image_height = 0;
    int thumb_padding = 0;
    int toolbar_height = 0;
    int scroll_y = 0;
};

struct GridTransitionRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

inline std::optional<GridEntryRequest> route_grid_entry(
    GridEntryTrigger trigger, const GridEntryRouteState& state) {
    if (!state.grid_mode || state.animating || state.item_count <= 0)
        return std::nullopt;
    const int index = trigger == GridEntryTrigger::Space
        ? state.selected_index : state.hit_index;
    if (index < 0 || index >= state.item_count) return std::nullopt;
    return GridEntryRequest{trigger, index};
}

inline bool route_grid_exit(
    GridExitTrigger trigger, const GridExitRouteState& state) {
    if (state.animating) return false;
    if (trigger == GridExitTrigger::DoubleClick) return state.has_image;
    return state.from_grid;
}

inline std::optional<GridTransitionRect> calculate_grid_transition_rect(
    const GridTransitionGeometry& geometry) {
    if (geometry.request_index < 0
        || geometry.request_index >= geometry.item_count
        || geometry.request_index < geometry.row_start_index
        || geometry.request_index >= geometry.row_end_index
        || geometry.row_height <= 0 || geometry.item_width <= 0.0f) {
        return std::nullopt;
    }

    const float image_width = geometry.image_width == 0
        ? 1.0f : static_cast<float>(geometry.image_width);
    const float image_height = geometry.image_height == 0
        ? 1.0f : static_cast<float>(geometry.image_height);
    const float center_x = geometry.item_x + geometry.thumb_padding
        + geometry.item_width * 0.5f;
    const float center_y = static_cast<float>(geometry.toolbar_height
        + geometry.row_y - geometry.scroll_y)
        + geometry.row_height * 0.5f;
    const float source_height = std::sqrt(
        geometry.item_width * geometry.row_height / (image_width / image_height));
    const float source_width = source_height * image_width / image_height;
    if (!std::isfinite(source_width) || !std::isfinite(source_height)
        || source_width <= 0.0f || source_height <= 0.0f) {
        return std::nullopt;
    }

    return GridTransitionRect{
        center_x - source_width * 0.5f,
        center_y - source_height * 0.5f,
        center_x + source_width * 0.5f,
        center_y + source_height * 0.5f};
}

template <typename Capture>
inline bool run_best_effort_transition_capture(Capture capture) noexcept {
    try {
        capture();
        return true;
    } catch (...) {
        return false;
    }
}

enum class ImageLoadResult {
    Success,
    DecodeFailed,
    MaterializeFailed,
    UploadFailed,
};

template <typename Decode, typename Materialize, typename Upload>
inline ImageLoadResult run_image_load_stages(
    Decode decode, Materialize materialize, Upload upload) noexcept {
    try {
        auto decoded = decode();
        if (!decoded) return ImageLoadResult::DecodeFailed;
        try {
            auto materialized = materialize(decoded);
            if (!materialized) return ImageLoadResult::MaterializeFailed;
            try {
                return upload(materialized)
                    ? ImageLoadResult::Success : ImageLoadResult::UploadFailed;
            } catch (...) {
                return ImageLoadResult::UploadFailed;
            }
        } catch (...) {
            return ImageLoadResult::MaterializeFailed;
        }
    } catch (...) {
        return ImageLoadResult::DecodeFailed;
    }
}

template <typename StartTransition, typename LoadAndCommit, typename BeginAnimation>
inline bool run_grid_entry(
    const GridEntryRequest& request,
    GridEntryTransactionState state,
    StartTransition start_transition,
    LoadAndCommit load_and_commit,
    BeginAnimation begin_animation) {
    if (request.index < 0 || !state.grid_mode || state.animating) return false;
    const bool initial_grid_mode = state.grid_mode;
    const bool initial_from_grid = state.from_grid;
    const bool initial_animating = state.animating;
    const int initial_grid_selection = state.grid_selection;
    const std::vector<bool> initial_selected = state.selected;
    const int initial_selection_anchor = state.selection_anchor;

    (void)run_best_effort_transition_capture(
        [&]() { start_transition(request.index); });

    bool loaded = false;
    try {
        loaded = load_and_commit(request.index);
    } catch (...) {
        loaded = false;
    }
    if (!loaded || state.grid_mode || !state.from_grid) {
        state.grid_mode = initial_grid_mode;
        state.from_grid = initial_from_grid;
        state.animating = initial_animating;
        state.grid_selection = initial_grid_selection;
        state.selected = initial_selected;
        state.selection_anchor = initial_selection_anchor;
        return false;
    }
    begin_animation();
    return !state.grid_mode && state.from_grid && state.animating;
}

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
