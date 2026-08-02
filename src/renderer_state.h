#pragma once

#include <algorithm>
#include <cstdint>
#include <winerror.h>

namespace mv {

inline bool should_recreate_render_device(HRESULT result) {
    return FAILED(result);
}

inline bool renderer_generation_changed(uint64_t cached, uint64_t current) {
    return cached != current;
}

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

} // namespace mv
