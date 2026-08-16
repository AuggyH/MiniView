#pragma once
// Pure album folder sampling (Maintainability Phase 1).
//
// Extracted from App::rebuild_folder_samples and path_under_folder in
// src/app.cpp. The model groups paths by the first matching folder and
// picks up to four representative samples per folder; the App keeps the
// m_folder_samples storage and the icon worker. No Windows or rendering
// dependencies.

#include <string>
#include <vector>

namespace mv {

/// Case-insensitive path-prefix test with a separator boundary, so
/// D:\A does not match D:\AIGC. Trailing separators on folder are ignored.
inline bool path_under_folder(const std::wstring& path,
    const std::wstring& folder) {
    size_t len = folder.size();
    while (len > 0
        && (folder[len - 1] == L'\\' || folder[len - 1] == L'/')) {
        --len;
    }
    if (len == 0 || path.size() < len) return false;
    if (_wcsnicmp(path.c_str(), folder.c_str(), len) != 0) return false;
    if (path.size() == len) return true;
    const wchar_t c = path[len];
    return c == L'\\' || c == L'/';
}

/// Group paths by the first matching folder (first-match wins) and sample
/// each group exactly like rebuild_folder_samples:
/// 1→[0], 2→[0,1], 3→[0,1,2], 4+→[0, k/4, k/2, 3k/4].
/// Returned vector is parallel to `folders`; empty groups yield no samples.
inline std::vector<std::vector<std::wstring>> sample_paths_per_folder(
    const std::vector<std::wstring>& paths,
    const std::vector<std::wstring>& folders) {
    std::vector<std::vector<std::wstring>> per_folder(folders.size());
    for (const auto& path : paths) {
        for (size_t f = 0; f < folders.size(); ++f) {
            if (path_under_folder(path, folders[f])) {
                per_folder[f].push_back(path);
                break;  // first matching folder wins
            }
        }
    }

    std::vector<std::vector<std::wstring>> samples(folders.size());
    for (size_t f = 0; f < folders.size(); ++f) {
        const auto& list = per_folder[f];
        const size_t k = list.size();
        if (k == 1) {
            samples[f].push_back(list[0]);
        } else if (k == 2) {
            samples[f].push_back(list[0]);
            samples[f].push_back(list[1]);
        } else if (k == 3) {
            samples[f].push_back(list[0]);
            samples[f].push_back(list[1]);
            samples[f].push_back(list[2]);
        } else if (k >= 4) {
            samples[f].push_back(list[0]);
            samples[f].push_back(list[k / 4]);
            samples[f].push_back(list[k / 2]);
            samples[f].push_back(list[3 * k / 4]);
        }
    }
    return samples;
}

} // namespace mv
