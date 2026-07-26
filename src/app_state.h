#pragma once

#include <algorithm>

namespace mv {

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

} // namespace mv
