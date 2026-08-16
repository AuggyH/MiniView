#pragma once
// Pure title-bar geometry model (Maintainability Phase 1).
//
// Extracted from the duplicated hit-testing arithmetic in src/app.cpp
// (WM_NCHITTEST, WM_LBUTTONDOWN, WM_MOUSEMOVE, show_toolbar_menu). The App
// still measures menu text with the renderer; everything that is pure
// geometry lives here so CTest can run it headless.

#include "layout.h"

#include <vector>

namespace mv {

struct TitleBarLayout {
    float x = 0.0f;         // left edge of the title bar, in physical px
    float width = 0.0f;     // title bar width, in physical px
    float height = 0.0f;    // title bar height, in physical px
    float dpi_scale = 1.0f; // dpi / 96
};

struct TitleBarMenuBound {
    float left = 0.0f;
    float right = 0.0f;
};

inline float title_bar_button_width(const TitleBarLayout& t) {
    return layout::kTitleBarButtonWidthDip * t.dpi_scale;
}

inline float title_bar_menu_start_x(const TitleBarLayout& t) {
    return t.x
        + (layout::kTitleBarPadDip
           + layout::kTitleBarTitleWidthDip
           + layout::kTitleBarTitleGapDip) * t.dpi_scale;
}

inline float title_bar_menu_font_size(const TitleBarLayout& t) {
    return layout::kTitleBarMenuFontSizeDip * t.dpi_scale;
}

inline float title_bar_menu_item_width(const TitleBarLayout& t,
    float measured_text_width) {
    return measured_text_width + layout::kTitleBarMenuPadDip * t.dpi_scale;
}

inline bool title_bar_contains_y(const TitleBarLayout& t, float y) {
    return y >= 0.0f && y < t.height;
}

/// Window button id under x: 0 = minimize, 1 = maximize, 2 = close,
/// -1 = none. Matches the original right-to-left 3-button checks.
inline int title_bar_window_button_at(const TitleBarLayout& t, float x) {
    const float bw = title_bar_button_width(t);
    const float right = t.x + t.width;
    if (x >= right - bw)       return 2;
    if (x >= right - 2 * bw)   return 1;
    if (x >= right - 3 * bw)   return 0;
    return -1;
}

/// Menu item index under x using float boundaries (WM_NCHITTEST and
/// show_toolbar_menu use this precision).
inline int title_bar_menu_item_at(const TitleBarLayout& t, float x,
    const std::vector<float>& measured_text_widths) {
    float mx = title_bar_menu_start_x(t);
    for (size_t i = 0; i < measured_text_widths.size(); ++i) {
        const float iw =
            title_bar_menu_item_width(t, measured_text_widths[i]);
        if (x >= mx && x < mx + iw) return static_cast<int>(i);
        mx += iw;
    }
    return -1;
}

/// Menu item index under integer x using the original int-truncated
/// boundaries (WM_LBUTTONDOWN and WM_MOUSEMOVE compare against
/// static_cast<int>(left/right)).
inline int title_bar_menu_item_at_integral(const TitleBarLayout& t, int x,
    const std::vector<float>& measured_text_widths) {
    float mx = title_bar_menu_start_x(t);
    for (size_t i = 0; i < measured_text_widths.size(); ++i) {
        const float iw =
            title_bar_menu_item_width(t, measured_text_widths[i]);
        if (x >= static_cast<int>(mx)
            && x < static_cast<int>(mx + iw)) {
            return static_cast<int>(i);
        }
        mx += iw;
    }
    return -1;
}

/// Precomputed menu item bounds for the popup hover-switch hook.
inline std::vector<TitleBarMenuBound> title_bar_menu_bounds(
    const TitleBarLayout& t,
    const std::vector<float>& measured_text_widths) {
    std::vector<TitleBarMenuBound> bounds;
    bounds.reserve(measured_text_widths.size());
    float mx = title_bar_menu_start_x(t);
    for (size_t i = 0; i < measured_text_widths.size(); ++i) {
        const float iw =
            title_bar_menu_item_width(t, measured_text_widths[i]);
        bounds.push_back({mx, mx + iw});
        mx += iw;
    }
    return bounds;
}

} // namespace mv
