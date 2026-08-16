#pragma once
// Pure selection-remapping helpers (Maintainability Phase 1).
//
// After the ImageIndex is rebuilt (recursive toggle, sort, copy, refresh),
// selection state must be re-located by path. These helpers own that path →
// index translation; the App applies the returned vectors to m_selected,
// m_sel_anchor and m_grid_sel. No Windows or rendering dependencies.

#include "indexer.h"

#include <string>
#include <vector>

namespace mv {

/// Translate paths to current index positions in the same order.
/// Paths not present in the index become -1; order is preserved.
inline std::vector<int> remap_paths_to_indices(const ImageIndex& index,
    const std::vector<std::wstring>& paths) {
    std::vector<int> indices;
    indices.reserve(paths.size());
    for (const auto& path : paths) {
        indices.push_back(index.index_of(path));
    }
    return indices;
}

/// Result of re-selecting previously selected paths after an index swap.
struct SelectionRemap {
    std::vector<int> selected;  // valid current indices, missing paths dropped
    int grid_sel = -1;          // remapped current grid selection
    int anchor = -1;            // remapped selection anchor (= grid_sel)
};

/// Rebuild grid selection state after an index rebuild:
/// - selected_path (the old m_grid_sel path) is remapped, or -1 when empty
/// - selected_before paths are remapped; missing paths are dropped
/// - anchor follows the remapped selected_path
inline SelectionRemap plan_selection_remap(const ImageIndex& index,
    const std::vector<std::wstring>& selected_before,
    const std::wstring& selected_path) {
    SelectionRemap remap;
    remap.grid_sel = selected_path.empty() ? -1 : index.index_of(selected_path);
    remap.selected.reserve(selected_before.size());
    for (const auto& path : selected_before) {
        const int idx = index.index_of(path);
        if (idx >= 0) remap.selected.push_back(idx);
    }
    remap.anchor = remap.grid_sel;
    return remap;
}

} // namespace mv
