#pragma once

#include "comic_reader_model.h"
#include "design_tokens.h"
#include "layout.h"
#include "navstate.h"

#include <d2d1.h>
#include <d2derr.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>
#include <winerror.h>

namespace mv {

constexpr std::size_t kComicOverlayMaxTextCharacters = 32768;

inline bool should_recreate_render_device(HRESULT result) {
    if (SUCCEEDED(result)) return false;
    // Recreate only on genuine device loss. Business errors (for example
    // EndDraw returning D2DERR_WRONG_STATE) must not tear the device down.
    return result == DXGI_ERROR_DEVICE_REMOVED
        || result == DXGI_ERROR_DEVICE_RESET
        || result == DXGI_ERROR_DEVICE_HUNG
        || result == D2DERR_RECREATE_TARGET;
}

inline bool renderer_generation_changed(uint64_t cached, uint64_t current) {
    return cached != current;
}

// Entry geometry, Quick Look-copied rhythm: reach ~90% size in the first
// 32% of the run, then settle to 100% with a quartic ease-out tail.
// The image stays fully opaque throughout.
inline float transition_ease(float t) {
    const float s = std::clamp(t, 0.0f, 1.0f);
    const float fast_end = dt::kTransitionEntryFastFraction;
    if (s < fast_end) {
        return dt::kTransitionEntryFastReach * (s / fast_end);
    }
    const float u = (s - fast_end) / (1.0f - fast_end);
    const float tail = 1.0f - (1.0f - u) * (1.0f - u) * (1.0f - u) * (1.0f - u);
    return dt::kTransitionEntryFastReach
        + (1.0f - dt::kTransitionEntryFastReach) * tail;
}

// Exit geometry, Quick Look-copied rhythm: hold the fitted rect for the
// first 60% (background veil reveals the grid), then collapse into the
// cell over the final 40% with a quartic ease-out. Fully opaque.
inline float transition_ease_exit(float t) {
    const float s = std::clamp(t, 0.0f, 1.0f);
    const float hold_end = dt::kTransitionExitHoldFraction;
    if (s < hold_end) return 0.0f;
    const float u = (s - hold_end) / (1.0f - hold_end);
    const float one_minus = 1.0f - u;
    return 1.0f - one_minus * one_minus * one_minus * one_minus;
}

// Exit veil: symmetric ease-in-out over the hold phase, fully revealing
// the grid exactly when the image starts collapsing.
inline float transition_veil_exit(float t) {
    const float s = std::clamp(t, 0.0f, 1.0f);
    const float hold_end = dt::kTransitionExitHoldFraction;
    const float u = std::clamp(s / hold_end, 0.0f, 1.0f);
    return u * u * (3.0f - 2.0f * u);
}

struct ComicRenderRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;

    float width() const noexcept {
        const float value = right - left;
        return std::isfinite(value) && value > 0.0f ? value : 0.0f;
    }
    float height() const noexcept {
        const float value = bottom - top;
        return std::isfinite(value) && value > 0.0f ? value : 0.0f;
    }
    bool empty() const noexcept {
        return !std::isfinite(left) || !std::isfinite(top)
            || !std::isfinite(right) || !std::isfinite(bottom)
            || !std::isfinite(right - left) || !std::isfinite(bottom - top)
            || right <= left || bottom <= top;
    }
};

struct ToastRect {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};

struct PanelToastLayoutInput {
    float panel_x = 0.0f;
    float panel_y = 0.0f;
    float panel_width = 0.0f;
    float panel_height = 0.0f;
    float measured_text_width = 0.0f;
    float measured_text_height = 0.0f;
    uint32_t line_count = 0;
    float dpi = 96.0f;
};

struct PanelToastLayout {
    ToastRect bounds;
    ToastRect text_bounds;
    float maximum_text_width = 0.0f;
    float maximum_text_height = 0.0f;
    float corner_radius = 0.0f;
    bool single_line = false;
};

inline PanelToastLayout calculate_panel_toast_layout(
    const PanelToastLayoutInput& input) {
    const float scale = dt::scale(input.dpi);
    const float panel_width = std::max(0.0f, input.panel_width);
    const float panel_height = std::max(0.0f, input.panel_height);
    const float horizontal_edge_inset = std::min(
        dt::kSpaceXlDip * scale, panel_width * 0.5f);
    const float vertical_edge_inset = std::min(
        dt::kSpaceXlDip * scale, panel_height * 0.5f);
    const float horizontal_padding = dt::kSpace10Dip * scale;
    const float vertical_padding = dt::kSpace7Dip * scale;

    const float available_width = std::max(0.0f,
        panel_width - horizontal_edge_inset * 2.0f);
    const float maximum_width = std::min(dt::kSize240Dip * scale, available_width);
    const float desired_width = std::max(dt::kSize96Dip * scale,
        std::max(0.0f, input.measured_text_width)
            + horizontal_padding * 2.0f + 2.0f * scale);
    const float width = std::min(maximum_width, desired_width);

    const float available_height = std::max(0.0f,
        panel_height - vertical_edge_inset * 2.0f);
    const float desired_height = std::max(0.0f, input.measured_text_height)
        + vertical_padding * 2.0f;
    const float height = std::min(available_height, desired_height);

    const float left = input.panel_x + (panel_width - width) * 0.5f;
    const float bottom = input.panel_y + panel_height - vertical_edge_inset;
    const float top = bottom - height;
    const float horizontal_inset = std::min(horizontal_padding, width * 0.5f);
    const float vertical_inset = std::min(vertical_padding, height * 0.5f);

    PanelToastLayout result;
    result.bounds = {left, top, left + width, bottom};
    result.text_bounds = {
        left + horizontal_inset,
        top + vertical_inset,
        left + width - horizontal_inset,
        bottom - vertical_inset,
    };
    result.maximum_text_width = std::max(0.0f,
        maximum_width - horizontal_padding * 2.0f);
    result.maximum_text_height = std::max(0.0f,
        available_height - vertical_padding * 2.0f);
    result.corner_radius = std::min({
        dt::kSpaceXsDip * scale, width * 0.5f, height * 0.5f});
    result.single_line = input.line_count == 1;
    return result;
}

struct ComicRenderViewport {
    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float scroll_y = 0.0f;
    float dpi = 96.0f;
    bool seamless = false;
};

struct ComicRenderMetrics {
    float dpi_scale = 1.0f;
    float page_gap = dt::kSpaceMdDip;
    float corner_radius = dt::kSpaceXsDip;
    float card_border_width = 1.0f;
    float card_padding = dt::kSpaceLgDip;
    float error_font_size = dt::kFontSizeXlDip;
};

enum class ComicPageVisual {
    Placeholder,
    Bitmap,
    Error,
};

struct ComicPageRenderInput {
    ComicPageGeometry geometry;
    ComicPageVisual visual = ComicPageVisual::Placeholder;
};

struct ComicPageRenderCommand {
    std::size_t input_index = 0;
    int page_index = -1;
    ComicRenderRect destination;
    ComicRenderRect clip;
    ComicPageVisual visual = ComicPageVisual::Placeholder;
};

struct ComicRenderPlan {
    ComicRenderRect viewport;
    ComicRenderMetrics metrics;
    std::vector<ComicPageRenderCommand> pages;
};

enum class ComicScrollbarHit {
    None,
    PageBackward,
    Thumb,
    PageForward,
};

enum class ComicAutoscrollDirection {
    Stationary,
    Backward,
    Forward,
};

enum class ComicTransientOverlayKind {
    None,
    PageChange,
    Status,
};

struct ComicControlMetrics {
    float dpi_scale = 1.0f;
    float edge_margin = dt::kSpaceSmDip;
    float overlay_gap = dt::kSpaceSmDip;
    float scrollbar_zone_width = dt::kSpace20Dip;
    float scrollbar_track_width = dt::kSpace6Dip;
    float scrollbar_min_thumb = dt::kSpace2xlDip;
    float scrollbar_radius = 3.0f;
    float page_badge_height = dt::kSize28Dip;
    float page_badge_min_width = dt::kSize64Dip;
    float page_badge_padding = dt::kSpace10Dip;
    float page_badge_font_size = dt::kFontSizeMdDip;
    float page_toast_height = dt::kSize36Dip;
    float page_toast_max_width = dt::kSize480Dip;
    float page_toast_padding = dt::kSpace14Dip;
    float page_toast_font_size = dt::kFontSizeLgDip;
    float autoscroll_anchor_radius = dt::kSpace10Dip;
    float autoscroll_dead_zone_radius = 16.0f;
    float autoscroll_arrow_min_length = 18.0f;
    float autoscroll_arrow_max_length = 48.0f;
    float autoscroll_arrow_head = 5.0f;
    float autoscroll_stroke_width = 1.5f;
};

struct ComicScrollbarGeometry {
    bool visible = false;
    bool hovered = false;
    bool dragging = false;
    ComicRenderRect hit_bounds;
    ComicRenderRect track;
    ComicRenderRect thumb;
    float scroll_range = 0.0f;
    float thumb_travel = 0.0f;
};

struct ComicScrollbarDragResult {
    bool valid = false;
    float scroll_y = 0.0f;
};

struct ComicTextOverlayLayout {
    bool visible = false;
    ComicRenderRect bounds;
    // Borrowed from ComicControlsRenderInput. Do not retain this layout beyond
    // the lifetime of the snapshot's backing strings.
    std::wstring_view text;
};

// Explicit page-width drag slider (mouse-direct width control).
struct ComicWidthSliderLayout {
    bool visible = false;
    ComicRenderRect track;
    float thumb_x = 0.0f;
    float thumb_y = 0.0f;
    float thumb_radius = 0.0f;
};

struct ComicAutoscrollLayout {
    bool visible = false;
    ComicAutoscrollDirection direction = ComicAutoscrollDirection::Stationary;
    float anchor_x = 0.0f;
    float anchor_y = 0.0f;
    float dead_zone_radius = 0.0f;
    float arrow_tail_y = 0.0f;
    float arrow_tip_y = 0.0f;
    float arrow_head = 0.0f;
    float intensity = 0.0f;
};

// Coordinates use the same absolute client physical-pixel space as
// ComicRenderViewport and App pointer input. The content viewport must already
// exclude any visible toolbar and side panel; dpi scales DIP constants only.
// App owns every timer, input state, business index, and preformatted backing
// string. Renderer synchronously consumes one immutable snapshot and does not
// format, copy, or allocate owning text, and it retains neither string view. A
// layout returned by build_comic_controls_layout borrows the views and must not
// outlive them.
struct ComicControlsRenderInput {
    ComicRenderRect content_viewport;
    float dpi = 96.0f;
    float virtual_height = 0.0f;
    float scroll_y = 0.0f;
    bool scrollbar_hovered = false;
    bool scrollbar_dragging = false;
    int anchored_page_index = -1; // zero-based validity gate only
    int total_pages = 0; // validity gate only
    std::wstring_view page_badge_text;
    ComicTransientOverlayKind transient_kind =
        ComicTransientOverlayKind::None;
    std::wstring_view transient_text;
    bool middle_autoscroll_active = false;
    float autoscroll_anchor_x = 0.0f;
    float autoscroll_anchor_y = 0.0f;
    float autoscroll_pointer_x = 0.0f;
    float autoscroll_pointer_y = 0.0f;
    float width_factor = 1.0f;   // page-width slider position 0.5..2.0
    bool cruise_active = false;  // persistent cruise indicator
    bool cruise_paused = false;
};

struct ComicControlsLayout {
    ComicRenderRect viewport;
    ComicControlMetrics metrics;
    ComicScrollbarGeometry scrollbar;
    ComicTextOverlayLayout page_badge;
    ComicTextOverlayLayout transient_overlay;
    ComicAutoscrollLayout autoscroll;
    ComicWidthSliderLayout width_slider;
};

inline float normalize_render_dpi(float dpi) noexcept {
    return std::isfinite(dpi) && dpi > 0.0f ? dpi : 96.0f;
}

inline ComicRenderMetrics comic_render_metrics(
    float dpi, bool seamless) noexcept {
    const float scale = normalize_render_dpi(dpi) / 96.0f;
    return ComicRenderMetrics{
        scale,
        seamless ? 0.0f : dt::kSpaceMdDip * scale,
        seamless ? 0.0f : dt::kSpaceXsDip * scale,
        1.0f * scale,
        dt::kSpaceLgDip * scale,
        dt::kFontSizeXlDip * scale};
}

inline ComicRenderRect intersect_render_rects(
    ComicRenderRect first, ComicRenderRect second) noexcept {
    if (first.empty() || second.empty()) return {};
    ComicRenderRect result{
        std::max(first.left, second.left),
        std::max(first.top, second.top),
        std::min(first.right, second.right),
        std::min(first.bottom, second.bottom)};
    return result.empty() ? ComicRenderRect{} : result;
}

inline ComicControlMetrics comic_control_metrics(float dpi) noexcept {
    const float scale = normalize_render_dpi(dpi) / 96.0f;
    return {
        scale,
        dt::kSpaceSmDip * scale,
        dt::kSpaceSmDip * scale,
        dt::kSpace20Dip * scale,
        dt::kSpace6Dip * scale,
        dt::kSpace2xlDip * scale,
        3.0f * scale,
        dt::kSize28Dip * scale,
        dt::kSize64Dip * scale,
        dt::kSpace10Dip * scale,
        dt::kFontSizeMdDip * scale,
        dt::kSize36Dip * scale,
        dt::kSize480Dip * scale,
        dt::kSpace14Dip * scale,
        dt::kFontSizeLgDip * scale,
        dt::kSpace10Dip * scale,
        16.0f * scale,
        18.0f * scale,
        48.0f * scale,
        5.0f * scale,
        1.5f * scale};
}

inline bool comic_control_metrics_valid(
    const ComicControlMetrics& metrics) noexcept {
    return std::isfinite(metrics.dpi_scale) && metrics.dpi_scale > 0.0f
        && std::isfinite(metrics.edge_margin)
        && std::isfinite(metrics.overlay_gap)
        && std::isfinite(metrics.scrollbar_zone_width)
        && std::isfinite(metrics.scrollbar_track_width)
        && std::isfinite(metrics.scrollbar_min_thumb)
        && std::isfinite(metrics.scrollbar_radius)
        && std::isfinite(metrics.page_badge_height)
        && std::isfinite(metrics.page_badge_min_width)
        && std::isfinite(metrics.page_badge_padding)
        && std::isfinite(metrics.page_badge_font_size)
        && std::isfinite(metrics.page_toast_height)
        && std::isfinite(metrics.page_toast_max_width)
        && std::isfinite(metrics.page_toast_padding)
        && std::isfinite(metrics.page_toast_font_size)
        && std::isfinite(metrics.autoscroll_anchor_radius)
        && std::isfinite(metrics.autoscroll_dead_zone_radius)
        && std::isfinite(metrics.autoscroll_arrow_min_length)
        && std::isfinite(metrics.autoscroll_arrow_max_length)
        && std::isfinite(metrics.autoscroll_arrow_head)
        && std::isfinite(metrics.autoscroll_stroke_width);
}

inline ComicScrollbarGeometry build_comic_scrollbar_geometry(
    ComicRenderRect viewport, float virtual_height, float scroll_y,
    float dpi, bool hovered = false, bool dragging = false) noexcept {
    ComicScrollbarGeometry geometry;
    geometry.hovered = hovered;
    geometry.dragging = dragging;
    if (viewport.empty() || !std::isfinite(dpi) || dpi <= 0.0f
        || !std::isfinite(virtual_height) || !std::isfinite(scroll_y)
        || virtual_height <= viewport.height()) return geometry;

    const ComicControlMetrics metrics = comic_control_metrics(dpi);
    if (!comic_control_metrics_valid(metrics)) return geometry;
    const float track_top = viewport.top + metrics.edge_margin;
    const float track_bottom = viewport.bottom - metrics.edge_margin;
    const float track_height = track_bottom - track_top;
    const float track_right = viewport.right - metrics.edge_margin;
    const float track_left = track_right - metrics.scrollbar_track_width;
    const float hit_left = viewport.right - metrics.scrollbar_zone_width;
    if (!std::isfinite(track_top) || !std::isfinite(track_bottom)
        || !std::isfinite(track_left) || !std::isfinite(track_right)
        || !std::isfinite(hit_left) || track_height <= 0.0f
        || track_left <= hit_left || hit_left < viewport.left) return geometry;

    const float scroll_range = virtual_height - viewport.height();
    const float ratio = viewport.height() / virtual_height;
    const float proportional_height = track_height * ratio;
    if (!std::isfinite(scroll_range) || !std::isfinite(ratio)
        || !std::isfinite(proportional_height) || scroll_range <= 0.0f) {
        return geometry;
    }
    const float thumb_height = std::min(
        track_height, std::max(metrics.scrollbar_min_thumb, proportional_height));
    const float thumb_travel = track_height - thumb_height;
    const float clamped_scroll = std::clamp(scroll_y, 0.0f, scroll_range);
    const float thumb_top = thumb_travel > 0.0f
        ? track_top + thumb_travel * (clamped_scroll / scroll_range)
        : track_top;
    const float thumb_bottom = thumb_top + thumb_height;
    if (!std::isfinite(thumb_top) || !std::isfinite(thumb_bottom)) {
        return geometry;
    }

    geometry.visible = true;
    geometry.hit_bounds = {hit_left, track_top, viewport.right, track_bottom};
    geometry.track = {track_left, track_top, track_right, track_bottom};
    geometry.thumb = {track_left, thumb_top, track_right, thumb_bottom};
    geometry.scroll_range = scroll_range;
    geometry.thumb_travel = thumb_travel;
    return geometry;
}

inline ComicScrollbarHit hit_test_comic_scrollbar(
    const ComicScrollbarGeometry& geometry, float x, float y) noexcept {
    if (!geometry.visible || geometry.hit_bounds.empty()
        || geometry.thumb.empty() || !std::isfinite(x) || !std::isfinite(y)
        || x < geometry.hit_bounds.left || x >= geometry.hit_bounds.right
        || y < geometry.hit_bounds.top || y >= geometry.hit_bounds.bottom) {
        return ComicScrollbarHit::None;
    }
    if (y < geometry.thumb.top) return ComicScrollbarHit::PageBackward;
    if (y > geometry.thumb.bottom) return ComicScrollbarHit::PageForward;
    return ComicScrollbarHit::Thumb;
}

inline ComicScrollbarDragResult map_comic_scrollbar_drag(
    const ComicScrollbarGeometry& geometry, float pointer_y,
    float grab_offset_y) noexcept {
    ComicScrollbarDragResult result;
    if (!geometry.visible || geometry.track.empty() || geometry.thumb.empty()
        || !std::isfinite(pointer_y) || !std::isfinite(grab_offset_y)
        || !std::isfinite(geometry.scroll_range)
        || !std::isfinite(geometry.thumb_travel)
        || grab_offset_y < 0.0f || grab_offset_y > geometry.thumb.height()
        || geometry.scroll_range <= 0.0f || geometry.thumb_travel <= 0.0f) {
        return result;
    }
    const float requested_top = pointer_y - grab_offset_y;
    if (!std::isfinite(requested_top)) return result;
    const float clamped_top = std::clamp(
        requested_top, geometry.track.top,
        geometry.track.bottom - geometry.thumb.height());
    const float fraction = (clamped_top - geometry.track.top)
        / geometry.thumb_travel;
    const float scroll_y = fraction * geometry.scroll_range;
    if (!std::isfinite(scroll_y)) return result;
    result.valid = true;
    result.scroll_y = std::clamp(scroll_y, 0.0f, geometry.scroll_range);
    return result;
}

inline ComicControlsLayout build_comic_controls_layout(
    const ComicControlsRenderInput& input) {
    ComicControlsLayout layout;
    if (input.content_viewport.empty() || !std::isfinite(input.dpi)
        || input.dpi <= 0.0f) return layout;
    layout.viewport = input.content_viewport;
    layout.metrics = comic_control_metrics(input.dpi);
    if (!comic_control_metrics_valid(layout.metrics)) return ComicControlsLayout{};
    layout.scrollbar = build_comic_scrollbar_geometry(
        layout.viewport, input.virtual_height, input.scroll_y, input.dpi,
        input.scrollbar_hovered, input.scrollbar_dragging);

    const bool valid_page = input.anchored_page_index >= 0
        && input.total_pages > 0
        && input.anchored_page_index < input.total_pages;
    const bool page_badge = valid_page && !input.page_badge_text.empty()
        && input.page_badge_text.size() <= kComicOverlayMaxTextCharacters;
    if (page_badge) {
        layout.page_badge.text = input.page_badge_text;
        const float usable_right = layout.scrollbar.visible
            ? layout.scrollbar.hit_bounds.left - layout.metrics.overlay_gap
            : layout.viewport.right - layout.metrics.edge_margin;
        const float available_width = usable_right - layout.viewport.left
            - layout.metrics.edge_margin;
        const float character_width = 7.5f * layout.metrics.dpi_scale;
        const float desired_width = std::max(
            layout.metrics.page_badge_min_width,
            character_width * static_cast<float>(layout.page_badge.text.size())
                + 2.0f * layout.metrics.page_badge_padding);
        const float badge_width = std::min(desired_width, available_width);
        const float badge_height = std::min(
            layout.metrics.page_badge_height,
            layout.viewport.height() - 2.0f * layout.metrics.edge_margin);
        if (std::isfinite(badge_width) && std::isfinite(badge_height)
            && badge_width > 0.0f && badge_height > 0.0f) {
            const float bottom = layout.viewport.bottom
                - layout.metrics.edge_margin;
            layout.page_badge.bounds = {
                usable_right - badge_width, bottom - badge_height,
                usable_right, bottom};
            layout.page_badge.visible = !layout.page_badge.bounds.empty();
        }

    }

    const bool valid_transient_text = !input.transient_text.empty()
        && input.transient_text.size() <= kComicOverlayMaxTextCharacters;
    const bool page_change = valid_page && input.transient_kind
        == ComicTransientOverlayKind::PageChange && valid_transient_text;
    const bool status = input.transient_kind
        == ComicTransientOverlayKind::Status && valid_transient_text;
    if (page_change || status) {
        layout.transient_overlay.text = input.transient_text;
        const float toast_right = layout.scrollbar.visible
            ? layout.scrollbar.hit_bounds.left - layout.metrics.overlay_gap
            : layout.viewport.right - layout.metrics.edge_margin;
        const float toast_left = layout.viewport.left
            + layout.metrics.edge_margin;
        const float available = toast_right - toast_left;
        const float toast_width = std::min(
            layout.metrics.page_toast_max_width, available);
        const float toast_bottom = layout.page_badge.visible
            ? layout.page_badge.bounds.top - layout.metrics.overlay_gap
            : layout.viewport.bottom - layout.metrics.edge_margin;
        const float toast_height = std::min(
            layout.metrics.page_toast_height,
            toast_bottom - layout.viewport.top - layout.metrics.edge_margin);
        if (std::isfinite(toast_width) && std::isfinite(toast_height)
            && toast_width > 0.0f && toast_height > 0.0f) {
            const float center = toast_left + available * 0.5f;
            layout.transient_overlay.bounds = {
                center - toast_width * 0.5f, toast_bottom - toast_height,
                center + toast_width * 0.5f, toast_bottom};
            layout.transient_overlay.visible =
                !layout.transient_overlay.bounds.empty();
        }
    }

    if (input.middle_autoscroll_active
        && std::isfinite(input.autoscroll_anchor_x)
        && std::isfinite(input.autoscroll_anchor_y)
        && std::isfinite(input.autoscroll_pointer_x)
        && std::isfinite(input.autoscroll_pointer_y)) {
        const float safe_right = layout.scrollbar.visible
            ? layout.scrollbar.hit_bounds.left : layout.viewport.right;
        const float radius = layout.metrics.autoscroll_dead_zone_radius;
        if (input.autoscroll_anchor_x >= layout.viewport.left
            && input.autoscroll_anchor_x < safe_right
            && input.autoscroll_anchor_y >= layout.viewport.top
            && input.autoscroll_anchor_y < layout.viewport.bottom) {
            ComicAutoscrollLayout& autoscroll = layout.autoscroll;
            autoscroll.visible = true;
            autoscroll.anchor_x = input.autoscroll_anchor_x;
            autoscroll.anchor_y = input.autoscroll_anchor_y;
            autoscroll.dead_zone_radius = radius;
            autoscroll.arrow_head = layout.metrics.autoscroll_arrow_head;
            const float delta_y = input.autoscroll_pointer_y
                - input.autoscroll_anchor_y;
            const float distance = std::fabs(delta_y);
            if (distance > radius) {
                autoscroll.direction = delta_y < 0.0f
                    ? ComicAutoscrollDirection::Backward
                    : ComicAutoscrollDirection::Forward;
                const float speed_distance = std::min(
                    distance - radius,
                    240.0f * layout.metrics.dpi_scale);
                autoscroll.intensity = std::clamp(
                    speed_distance / (240.0f * layout.metrics.dpi_scale),
                    0.0f, 1.0f);
                const float length = layout.metrics.autoscroll_arrow_min_length
                    + (layout.metrics.autoscroll_arrow_max_length
                        - layout.metrics.autoscroll_arrow_min_length)
                        * autoscroll.intensity;
                const float sign = autoscroll.direction
                    == ComicAutoscrollDirection::Backward ? -1.0f : 1.0f;
                autoscroll.arrow_tail_y = autoscroll.anchor_y
                    + sign * (radius + layout.metrics.overlay_gap * 0.5f);
                const float unclipped_tip = autoscroll.arrow_tail_y
                    + sign * length;
                autoscroll.arrow_tip_y = std::clamp(
                    unclipped_tip,
                    layout.viewport.top + autoscroll.arrow_head,
                    layout.viewport.bottom - autoscroll.arrow_head);
                if ((sign < 0.0f
                        && autoscroll.arrow_tip_y >= autoscroll.arrow_tail_y)
                    || (sign > 0.0f
                        && autoscroll.arrow_tip_y <= autoscroll.arrow_tail_y)) {
                    autoscroll.direction = ComicAutoscrollDirection::Stationary;
                    autoscroll.intensity = 0.0f;
                }
            }
        }
    }

    // Page-width drag slider, bottom-right of the comic viewport.
    {
        const float scale = layout.metrics.dpi_scale;
        const float track_w = 160.0f * scale;
        const float track_h = 4.0f * scale;
        const float margin = 26.0f * scale;
        const float track_y = layout.viewport.bottom - margin;
        const float track_right = layout.viewport.right - margin;
        const float track_left = track_right - track_w;
        if (track_left > layout.viewport.left + margin) {
            layout.width_slider.visible = true;
            layout.width_slider.track = {
                track_left, track_y - track_h * 0.5f,
                track_right, track_y + track_h * 0.5f};
            const float t = (input.width_factor - 0.5f) / 1.5f;
            layout.width_slider.thumb_x = track_left
                + std::clamp(t, 0.0f, 1.0f) * track_w;
            layout.width_slider.thumb_y = track_y;
            layout.width_slider.thumb_radius = 7.0f * scale;
        }
    }
    return layout;
}

inline bool hit_test_comic_width_slider(
    const ComicWidthSliderLayout& slider, float x, float y) {
    if (!slider.visible) return false;
    const float pad = 12.0f;
    return x >= slider.track.left - pad && x <= slider.track.right + pad
        && y >= slider.track.top - pad && y <= slider.track.bottom + pad;
}

inline ComicRenderPlan build_comic_render_plan(
    std::span<const ComicPageRenderInput> inputs,
    ComicRenderViewport viewport) {
    ComicRenderPlan plan;
    plan.metrics = comic_render_metrics(viewport.dpi, viewport.seamless);
    if (!std::isfinite(viewport.left) || !std::isfinite(viewport.top)
        || !std::isfinite(viewport.width) || !std::isfinite(viewport.height)
        || !std::isfinite(viewport.scroll_y)
        || viewport.width <= 0.0f || viewport.height <= 0.0f) return plan;

    const float viewport_right = viewport.left + viewport.width;
    const float viewport_bottom = viewport.top + viewport.height;
    if (!std::isfinite(viewport_right) || !std::isfinite(viewport_bottom)) {
        return plan;
    }
    plan.viewport = {
        viewport.left,
        viewport.top,
        viewport_right,
        viewport_bottom};
    if (plan.viewport.empty()) return plan;

    plan.pages.reserve(inputs.size());
    for (std::size_t input_index = 0; input_index < inputs.size(); ++input_index) {
        const ComicPageRenderInput& input = inputs[input_index];
        const ComicPageGeometry& geometry = input.geometry;
        if (!std::isfinite(geometry.left) || !std::isfinite(geometry.top)
            || !std::isfinite(geometry.width) || !std::isfinite(geometry.height)
            || geometry.width <= 0.0f || geometry.height <= 0.0f) continue;

        const float left = viewport.left + geometry.left;
        const float top = viewport.top + geometry.top - viewport.scroll_y;
        const float right = left + geometry.width;
        const float bottom = top + geometry.height;
        const ComicRenderRect destination{
            left, top, right, bottom};
        if (destination.empty()) continue;
        const ComicRenderRect clip = intersect_render_rects(
            destination, plan.viewport);
        if (clip.empty()) continue;

        const ComicPageVisual visual = geometry.decode_failed
            ? ComicPageVisual::Error : input.visual;
        plan.pages.push_back({
            input_index, geometry.index, destination, clip, visual});
    }
    return plan;
}

// ── Left navigation panel render geometry (Issue #5 P2) ─────

struct NavPanelGeometry {
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    float breadcrumb_y = 0.0f;
    float breadcrumb_h = 0.0f;
    float tabs_y = 0.0f;
    float tabs_h = 0.0f;
    float tree_x = 0.0f;
    float tree_y = 0.0f;
    float tree_w = 0.0f;
    float tree_h = 0.0f;
    float scrollbar_x = 0.0f;
    float scrollbar_w = 0.0f;
    float stats_y = 0.0f;
    float stats_h = 0.0f;
    float toggle_x = 0.0f;   // album view-mode toggle button (tree/icons)
    float toggle_y = 0.0f;
    float toggle_w = 0.0f;
    float toggle_h = 0.0f;
};

inline NavPanelGeometry build_nav_panel_geometry(
    float x, float y, float w, float h, float dpi_scale) {
    NavPanelGeometry g;
    g.x = x;
    g.y = y;
    g.w = w;
    g.h = h;
    g.breadcrumb_h = layout::kNavBreadcrumbBarHeightDip * dpi_scale;
    g.breadcrumb_y = y;
    g.tabs_h = layout::kNavTabHeightDip * dpi_scale;
    g.tabs_y = g.breadcrumb_y + g.breadcrumb_h;
    g.stats_h = layout::kNavStatsHeightDip * dpi_scale;
    g.stats_y = y + h - g.stats_h;
    g.tree_x = x;
    g.tree_y = g.tabs_y + g.tabs_h;
    g.tree_h = std::max(0.0f, g.stats_y - g.tree_y);
    g.tree_w = w;
    g.scrollbar_w = std::max(4.0f, layout::kNavScrollbarWidthDip * dpi_scale);
    g.scrollbar_x = x + w - g.scrollbar_w;
    g.toggle_w = layout::kNavToggleButtonWidthDip * dpi_scale;
    g.toggle_h = layout::kNavTabHeightDip * dpi_scale * 0.72f;
    g.toggle_x = x + w - g.scrollbar_w - 4.0f * dpi_scale - g.toggle_w;
    g.toggle_y = g.tabs_y + (g.tabs_h - g.toggle_h) * 0.5f;
    return g;
}

struct NavBreadcrumbRenderInput {
    float x = 0.0f;            // strip left edge (absolute client coords)
    float y = 0.0f;            // strip top edge
    float width = 0.0f;        // strip width (for the separator line)
    float height = 0.0f;
    const NavBreadcrumbLayout* layout = nullptr;
    const std::vector<std::wstring>* segments = nullptr;
    int hover_item = -1;       // breadcrumb item index under cursor
    float dpi_scale = 1.0f;
};

// Album/favourite panel row (Issue #5 P3). One flat list: the fixed
// favourite row, user albums, and the selected album's folders.
struct AlbumPanelRow {
    enum class Kind { Favourites, Album, Folder };
    Kind kind = Kind::Album;
    std::wstring name;
    int depth = 0;           // folder indent level
    bool recursive = false;  // folder recursive badge
    bool error = false;      // stale path (missing directory)
    int image_count = -1;    // -1 = hidden
    bool selected = false;   // active collection row
    int album_index = -1;    // owning album (Album/Folder rows)
    int folder_index = -1;   // folder position (Folder rows)
};

// Folder-icon collage tiles (Issue #5 P3c): up to 4 sampled thumbs per
// folder, uploaded as D2D bitmaps by the folder-icon worker.
struct FolderIconTiles {
    std::vector<Microsoft::WRL::ComPtr<ID2D1Bitmap>> tiles;
};

struct FolderIconCell {
    int row_index = -1;   // AlbumPanelRow index (Folder kind)
    float x = 0.0f;
    float y = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    float label_y = 0.0f;
};

// Icon-grid layout for the selected album\'s folder rows: list rows
// (favourites + albums) keep the tree layout; folder rows become square
// cells with 2 or 3 per row. Returns cells in row order.
inline std::vector<FolderIconCell> build_folder_icon_layout(
    const std::vector<AlbumPanelRow>& rows, const NavPanelGeometry& g,
    int cols, float dpi_scale) {
    std::vector<FolderIconCell> cells;
    int list_count = 0;
    while (list_count < static_cast<int>(rows.size())
        && rows[static_cast<size_t>(list_count)].kind
            != AlbumPanelRow::Kind::Folder)
        ++list_count;
    const float row_h = layout::kNavRowHeightDip * dpi_scale;
    const float pad = layout::kNavPadDip * dpi_scale;
    const float grid_y = g.tree_y + pad
        + static_cast<float>(list_count) * row_h
        + layout::kNavPadDip * dpi_scale;
    const float gap = layout::kNavPadDip * dpi_scale;
    const float avail = g.tree_w - g.scrollbar_w
        - pad * 2.0f - 4.0f * dpi_scale;
    const float cell_w = std::max(24.0f,
        (avail - gap * static_cast<float>(cols - 1))
            / static_cast<float>(cols));
    const float label_h = 18.0f * dpi_scale;
    int index = 0;
    for (int i = list_count; i < static_cast<int>(rows.size()); ++i) {
        FolderIconCell cell;
        cell.row_index = i;
        cell.w = cell_w;
        cell.h = cell_w;
        cell.x = g.tree_x + pad
            + static_cast<float>(index % cols) * (cell_w + gap);
        cell.y = grid_y
            + static_cast<float>(index / cols) * (cell_w + label_h + gap);
        cell.label_y = cell.y + cell.h + 2.0f * dpi_scale;
        cells.push_back(cell);
        ++index;
    }
    return cells;
}

struct NavPanelRenderInput {
    NavPanelGeometry geometry;
    const NavBreadcrumbLayout* breadcrumb = nullptr;
    const std::vector<std::wstring>* segments = nullptr;
    int breadcrumb_hover = -1;
    NavPanelTab tab = NavPanelTab::Directories;
    const std::vector<NavTreeRow>* rows = nullptr;
    int row_hover = -1;        // visible row index under cursor
    std::uint64_t highlight_id = 0;   // active collection node id
    bool highlight_recursive = false; // active collection is recursive → badge
    float tree_scroll = 0.0f;
    float tree_total = 0.0f;
    bool tree_scroll_active = false;
    const std::wstring* stats_text = nullptr;  // bottom stats line
    float dpi_scale = 1.0f;
    // Album/favourite tab (Issue #5 P3)
    const std::vector<AlbumPanelRow>* album_rows = nullptr;
    int album_row_hover = -1;
    int icons_mode = 0;        // 0 = tree, 2 = icon grid 2 cols, 3 = 3 cols
    const std::vector<FolderIconTiles>* folder_tiles = nullptr;  // parallel to album_rows
};

} // namespace mv
