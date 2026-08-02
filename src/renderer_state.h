#pragma once

#include "comic_reader_model.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>
#include <winerror.h>

namespace mv {

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

    float width() const noexcept { return std::max(0.0f, right - left); }
    float height() const noexcept { return std::max(0.0f, bottom - top); }
    bool empty() const noexcept {
        return !std::isfinite(left) || !std::isfinite(top)
            || !std::isfinite(right) || !std::isfinite(bottom)
            || right <= left || bottom <= top;
    }
};

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
