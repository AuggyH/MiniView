#pragma once

#include "comic_reader_model.h"

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
    return FAILED(result);
}

inline bool renderer_generation_changed(uint64_t cached, uint64_t current) {
    return cached != current;
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
    const float scale = input.dpi > 0.0f ? input.dpi / 96.0f : 1.0f;
    const float panel_width = std::max(0.0f, input.panel_width);
    const float panel_height = std::max(0.0f, input.panel_height);
    const float horizontal_edge_inset = std::min(
        24.0f * scale, panel_width * 0.5f);
    const float vertical_edge_inset = std::min(
        24.0f * scale, panel_height * 0.5f);
    const float horizontal_padding = 10.0f * scale;
    const float vertical_padding = 7.0f * scale;

    const float available_width = std::max(0.0f,
        panel_width - horizontal_edge_inset * 2.0f);
    const float maximum_width = std::min(240.0f * scale, available_width);
    const float desired_width = std::max(96.0f * scale,
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
        4.0f * scale, width * 0.5f, height * 0.5f});
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
    float page_gap = 12.0f;
    float corner_radius = 4.0f;
    float card_border_width = 1.0f;
    float card_padding = 16.0f;
    float error_font_size = 14.0f;
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
    float edge_margin = 8.0f;
    float overlay_gap = 8.0f;
    float scrollbar_zone_width = 20.0f;
    float scrollbar_track_width = 6.0f;
    float scrollbar_min_thumb = 32.0f;
    float scrollbar_radius = 3.0f;
    float page_badge_height = 28.0f;
    float page_badge_min_width = 64.0f;
    float page_badge_padding = 10.0f;
    float page_badge_font_size = 12.0f;
    float page_toast_height = 36.0f;
    float page_toast_max_width = 480.0f;
    float page_toast_padding = 14.0f;
    float page_toast_font_size = 13.0f;
    float autoscroll_anchor_radius = 10.0f;
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
};

struct ComicControlsLayout {
    ComicRenderRect viewport;
    ComicControlMetrics metrics;
    ComicScrollbarGeometry scrollbar;
    ComicTextOverlayLayout page_badge;
    ComicTextOverlayLayout transient_overlay;
    ComicAutoscrollLayout autoscroll;
};

inline float normalize_render_dpi(float dpi) noexcept {
    return std::isfinite(dpi) && dpi > 0.0f ? dpi : 96.0f;
}

inline ComicRenderMetrics comic_render_metrics(
    float dpi, bool seamless) noexcept {
    const float scale = normalize_render_dpi(dpi) / 96.0f;
    return ComicRenderMetrics{
        scale,
        seamless ? 0.0f : 12.0f * scale,
        seamless ? 0.0f : 4.0f * scale,
        1.0f * scale,
        16.0f * scale,
        14.0f * scale};
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
        8.0f * scale,
        8.0f * scale,
        20.0f * scale,
        6.0f * scale,
        32.0f * scale,
        3.0f * scale,
        28.0f * scale,
        64.0f * scale,
        10.0f * scale,
        12.0f * scale,
        36.0f * scale,
        480.0f * scale,
        14.0f * scale,
        13.0f * scale,
        10.0f * scale,
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
    return layout;
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

} // namespace mv
