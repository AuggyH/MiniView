#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mv {

inline constexpr std::uint32_t kComicAppTimerIntervalMs = 16;
inline constexpr std::uint64_t kComicAppTransientDurationMs = 1000;

enum class GridRebuildReason {
    None,
    Structural,
    BackgroundDimensions,
};

enum class ComicAppCommand {
    ToggleCruise,
    SetSpeed05,
    SetSpeed10,
    SetSpeed15,
    SetSpeed20,
    DecreaseSpeed,
    IncreaseSpeed,
};

enum class ComicAppAutoOwner {
    None,
    Cruise,
    Middle,
};

enum class ComicAppCancelTrigger {
    ManualInput,
    Scrollbar,
    RepeatedMiddleClick,
    LeftButton,
    Escape,
    KeyboardPage,
    MouseWheel,
    FocusLost,
    ExitMode,
    EmptyBook,
    ViewportChanged,
    InvalidInput,
};

enum class ComicAppCruiseStatus {
    Speed,
    Paused,
    Boundary,
};

// Port is the single production seam for comic command/input/timer lifecycle.
// App supplies the real Model/Win32 effects; tests supply a recording fake.
class ComicAppController {
public:
    template <typename Port>
    static bool dispatch_command(Port& port, ComicAppCommand command) {
        if (!port.enabled()) return false;
        if (command == ComicAppCommand::ToggleCruise) {
            const ComicAppAutoOwner previous = port.owner();
            const bool active = port.toggle_cruise();
            if (previous == ComicAppAutoOwner::Middle
                && port.owner() != ComicAppAutoOwner::Middle) {
                port.release_middle_capture();
                port.set_middle_cursor(false);
            }
            if (active) {
                port.begin_tick_clock();
                port.show_cruise_status(ComicAppCruiseStatus::Speed);
            } else {
                port.show_cruise_status(previous == ComicAppAutoOwner::Cruise
                    ? ComicAppCruiseStatus::Paused
                    : ComicAppCruiseStatus::Boundary);
            }
            (void)ensure_timer(port);
            port.invalidate();
            return true;
        }

        int requested_speed = port.speed_index();
        switch (command) {
        case ComicAppCommand::SetSpeed05: requested_speed = 0; break;
        case ComicAppCommand::SetSpeed10: requested_speed = 1; break;
        case ComicAppCommand::SetSpeed15: requested_speed = 2; break;
        case ComicAppCommand::SetSpeed20: requested_speed = 3; break;
        case ComicAppCommand::DecreaseSpeed: --requested_speed; break;
        case ComicAppCommand::IncreaseSpeed: ++requested_speed; break;
        case ComicAppCommand::ToggleCruise: break;
        }
        port.set_speed(requested_speed);
        port.show_cruise_status(ComicAppCruiseStatus::Speed);
        (void)ensure_timer(port);
        port.invalidate();
        return true;
    }

    template <typename Port>
    static bool start_middle(
        Port& port, float anchor_x, float anchor_y,
        float pointer_x, float pointer_y, bool anchor_visible) {
        if (!port.enabled()) return false;
        if (port.owner() == ComicAppAutoOwner::Middle) {
            return cancel(port, ComicAppCancelTrigger::RepeatedMiddleClick);
        }
        if (!anchor_visible
            || !port.start_middle(anchor_x, anchor_y, pointer_x, pointer_y)) {
            return false;
        }
        port.clear_status_transient();
        port.begin_tick_clock();
        if (!port.acquire_middle_capture()) {
            (void)cancel(port, ComicAppCancelTrigger::InvalidInput);
            return false;
        }
        port.set_middle_cursor(true);
        if (!ensure_timer(port)) return false;
        port.invalidate();
        return true;
    }

    template <typename Port>
    static bool cancel(Port& port, ComicAppCancelTrigger trigger) {
        const ComicAppAutoOwner previous = port.owner();
        if (previous == ComicAppAutoOwner::None) return false;
        port.cancel_auto_scroll(trigger);
        port.clear_status_transient();
        if (previous == ComicAppAutoOwner::Middle) {
            port.release_middle_capture();
        }
        stop_timer_if_idle(port);
        if (previous == ComicAppAutoOwner::Middle) {
            port.set_middle_cursor(false);
        }
        port.invalidate();
        return true;
    }

    template <typename Port>
    static bool timer_tick(Port& port, float elapsed_seconds, bool transient_expired) {
        if (!port.timer_running()) return false;
        const ComicAppAutoOwner previous = port.owner();
        float applied = 0.0f;
        if (previous == ComicAppAutoOwner::Cruise) {
            applied = port.advance_cruise(elapsed_seconds);
        } else if (previous == ComicAppAutoOwner::Middle) {
            applied = port.advance_middle(elapsed_seconds);
        }
        bool redraw = false;
        if (applied != 0.0f) {
            port.sync_page();
            port.request_pages();
            redraw = true;
        }
        const bool middle_stopped = previous == ComicAppAutoOwner::Middle
            && port.owner() != ComicAppAutoOwner::Middle;
        if (middle_stopped) {
            port.release_middle_capture();
            redraw = true;
        }
        if (transient_expired) {
            port.clear_all_transient();
            redraw = true;
        }
        stop_timer_if_idle(port);
        if (middle_stopped) port.set_middle_cursor(false);
        if (redraw) port.invalidate();
        return redraw;
    }

    template <typename Port>
    static bool transient_changed(Port& port) {
        if (!port.transient_visible()) return false;
        if (!ensure_timer(port)) return false;
        port.invalidate();
        return true;
    }

    template <typename Port>
    static bool viewport_changed(Port& port, bool middle_anchor_visible) {
        if (port.owner() != ComicAppAutoOwner::Middle
            || middle_anchor_visible) {
            return false;
        }
        return cancel(port, ComicAppCancelTrigger::ViewportChanged);
    }

    template <typename Port>
    static void stop_timer_if_idle(Port& port) {
        if (port.owner() == ComicAppAutoOwner::None
            && !port.transient_visible()) {
            port.stop_timer();
        }
    }

private:
    template <typename Port>
    static bool ensure_timer(Port& port) {
        if (port.timer_running() || port.start_timer()) return true;
        const ComicAppAutoOwner previous = port.owner();
        port.clear_all_transient();
        if (previous != ComicAppAutoOwner::None) {
            port.cancel_auto_scroll(ComicAppCancelTrigger::InvalidInput);
        }
        if (previous == ComicAppAutoOwner::Middle) {
            port.release_middle_capture();
        }
        port.stop_timer();
        if (previous == ComicAppAutoOwner::Middle) {
            port.set_middle_cursor(false);
        }
        port.invalidate();
        return false;
    }
};

enum class RecursiveScanAction {
    KeepView,
    RefreshGrid,
    EnterUnselectedGrid,
    ShowEmptyRoot,
};

inline bool can_toggle_recursive(
    bool grid_mode, bool has_image, const std::wstring& root_directory) {
    return grid_mode || (!has_image && !root_directory.empty());
}

inline RecursiveScanAction classify_recursive_scan_action(
    bool was_grid, bool has_image, int scan_result) {
    if (was_grid && scan_result == 0)
        return RecursiveScanAction::ShowEmptyRoot;
    if (was_grid) return RecursiveScanAction::RefreshGrid;
    if (scan_result > 0 && !has_image)
        return RecursiveScanAction::EnterUnselectedGrid;
    return RecursiveScanAction::KeepView;
}

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

class GridScrollPause {
public:
    bool active() const noexcept { return m_active; }
    std::uintptr_t timer() const noexcept { return m_timer; }

    template <typename CancelTimer, typename StartTimer>
    void begin(CancelTimer cancel_timer, StartTimer start_timer) {
        finish(cancel_timer);
        m_active = true;
        m_timer = static_cast<std::uintptr_t>(start_timer());
        if (m_timer == 0) m_active = false;
    }

    template <typename CancelTimer>
    void finish(CancelTimer cancel_timer) {
        if (m_timer != 0) cancel_timer(m_timer);
        m_timer = 0;
        m_active = false;
    }

    template <typename RequestThumbnail>
    void request_visible(
        bool loader_running, int first, int last,
        RequestThumbnail request_thumbnail) const {
        if (m_active || !loader_running) return;
        for (int index = first; index < last; ++index)
            request_thumbnail(index);
    }

private:
    bool m_active = false;
    std::uintptr_t m_timer = 0;
};

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

template <typename Decode, typename Materialize, typename Upload>
inline bool run_image_load_stages(
    Decode decode, Materialize materialize, Upload upload) noexcept {
    try {
        auto decoded = decode();
        auto materialized = materialize(decoded);
        return upload(materialized);
    } catch (...) {
        return false;
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

inline bool apply_grid_label_toggle(
    bool grid_mode, bool& show_labels, bool& layout_dirty) {
    if (!grid_mode) return false;
    show_labels = !show_labels;
    layout_dirty = true;
    return true;
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
