#pragma once
// Pure grid layout model (Maintainability Phase 1).
//
// Extracted from App::rebuild_grid_layout in src/app.cpp. The model owns the
// row/column arithmetic and item x/width computation; the App keeps anchor
// preservation, scroll reconciliation, and copying the results into its
// members. No Windows or rendering dependencies — CTest can run it headless.

#include "layout.h"

#include <algorithm>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace mv {

struct GridLayoutInput {
    int item_count = 0;
    int area_width = 0;
    int cell = 0;          // effective thumbnail cell size (zoom applied)
    int gap_h = 0;
    int gap_v = 0;
    int pad = 0;           // m_thumb_pad: uniform grid padding / start offset
    bool square = false;
    bool show_labels = true;
    float dpi_scale = 1.0f;
    std::span<const std::pair<uint32_t, uint32_t>> dims;
};

struct GridLayoutRow {
    int start_idx = 0;
    int end_idx = 0;
    int row_h = 0;
    int row_y = 0;
    int label_extra = 0;
};

struct GridLayout {
    int cols = 0;
    std::vector<GridLayoutRow> rows;
    std::vector<float> item_x;
    std::vector<float> item_w;
    int total_height = 0;
};

inline int grid_layout_label_height(const GridLayoutInput& input) {
    return input.show_labels
        ? static_cast<int>(layout::kGridLabelHeightDip * input.dpi_scale)
        : 0;
}

inline GridLayout plan_grid_layout(const GridLayoutInput& input) {
    GridLayout layout;
    const int total = input.item_count;
    const int effective_cell = std::max(1, input.cell);
    const int cols = std::max(1,
        (input.area_width + input.gap_h) / (effective_cell + input.gap_h));
    const int label_height = grid_layout_label_height(input);

    layout.cols = cols;
    layout.rows.reserve(
        static_cast<size_t>((total + cols - 1) / cols));
    layout.item_x.assign(static_cast<size_t>(total), 0.0f);
    layout.item_w.assign(static_cast<size_t>(total), 0.0f);

    int current_y = input.pad;
    if (input.square) {
        const int cell_width = std::max(effective_cell,
            (input.area_width - (cols - 1) * input.gap_h) / cols);
        const int x0 = std::max(0,
            (input.area_width - cols * cell_width - (cols - 1) * input.gap_h)
                / 2);
        for (int index = 0; index < total; index += cols) {
            GridLayoutRow row;
            row.start_idx = index;
            row.end_idx = std::min(index + cols, total);
            row.row_h = cell_width;
            row.row_y = current_y;
            row.label_extra = label_height;
            float x = static_cast<float>(x0);
            for (int i = row.start_idx; i < row.end_idx; ++i) {
                layout.item_x[static_cast<size_t>(i)] = x;
                layout.item_w[static_cast<size_t>(i)] =
                    static_cast<float>(cell_width);
                x += cell_width + input.gap_h;
            }
            layout.rows.push_back(row);
            current_y += row.row_h + input.gap_v + row.label_extra;
        }
    } else {
        const int usable_width =
            std::max(1, input.area_width - (cols - 1) * input.gap_h);
        constexpr float base_height = 120.0f;
        for (int index = 0; index < total; index += cols) {
            GridLayoutRow row;
            row.start_idx = index;
            row.end_idx = std::min(index + cols, total);
            row.row_y = current_y;
            row.label_extra = label_height;
            // 行级冻结: 本行只要有一个尺寸未知, 整行按 1:1 排; 全部就绪后
            // 该行一次性换成真实比例。避免每张图尺寸到达都重排造成漂移。
            bool row_ready = true;
            for (int i = row.start_idx; i < row.end_idx; ++i) {
                const auto dim = input.dims[static_cast<size_t>(i)];
                if (dim.first == 0 || dim.second == 0) {
                    row_ready = false;
                    break;
                }
            }
            double width_at_base = 0.0;
            for (int i = row.start_idx; i < row.end_idx; ++i) {
                const auto [raw_w, raw_h] =
                    input.dims[static_cast<size_t>(i)];
                const uint32_t image_w = !row_ready || raw_w == 0 ? 1 : raw_w;
                const uint32_t image_h = !row_ready || raw_h == 0 ? 1 : raw_h;
                width_at_base += static_cast<double>(base_height)
                    * image_w / image_h;
            }
            const float scale = width_at_base > 0.0
                ? static_cast<float>(usable_width / width_at_base)
                : 1.0f;
            row.row_h = std::max(40, static_cast<int>(base_height * scale));
            float x = 0.0f;
            for (int i = row.start_idx; i < row.end_idx; ++i) {
                const auto [raw_w, raw_h] =
                    input.dims[static_cast<size_t>(i)];
                const uint32_t image_w = !row_ready || raw_w == 0 ? 1 : raw_w;
                const uint32_t image_h = !row_ready || raw_h == 0 ? 1 : raw_h;
                const float display_width =
                    static_cast<float>(row.row_h) * image_w / image_h;
                layout.item_x[static_cast<size_t>(i)] = x;
                layout.item_w[static_cast<size_t>(i)] = display_width;
                x += display_width + input.gap_h;
            }
            layout.rows.push_back(row);
            current_y += row.row_h + input.gap_v + row.label_extra;
        }
    }

    layout.total_height = current_y;
    return layout;
}

} // namespace mv
