#include "indexer.h"
#include <filesystem>
#include <algorithm>
#include <cwchar>

namespace mv {
namespace fs = std::filesystem;

namespace {
    const std::wstring EMPTY_STR;
}

static bool is_image_ext(const std::wstring& ext) {
    return ext == L".png"  || ext == L".jpg"  || ext == L".jpeg" ||
           ext == L".bmp"  || ext == L".gif"  || ext == L".webp" ||
           ext == L".tiff" || ext == L".tif";
}

void ImageIndex::clear() {
    m_files.clear();
    m_path_to_idx.clear();
    m_root_dir.clear();
}

// ── Sort implementations ─────────────────────────────────────

void ImageIndex::sort_by_name() {
    std::sort(m_files.begin(), m_files.end(),
        [](const ImageEntry& a, const ImageEntry& b) {
            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });
    rebuild_map();
}

void ImageIndex::sort_by_path() {
    std::sort(m_files.begin(), m_files.end(),
        [](const ImageEntry& a, const ImageEntry& b) {
            return _wcsicmp(a.path.c_str(), b.path.c_str()) < 0;
        });
    rebuild_map();
}

void ImageIndex::sort_by_date() {
    std::sort(m_files.begin(), m_files.end(),
        [](const ImageEntry& a, const ImageEntry& b) {
            return a.mtime > b.mtime;  // newest first
        });
    rebuild_map();
}

void ImageIndex::sort_by_size() {
    std::sort(m_files.begin(), m_files.end(),
        [](const ImageEntry& a, const ImageEntry& b) {
            return a.size > b.size;  // largest first
        });
    rebuild_map();
}

void ImageIndex::sort_random() {
    std::shuffle(m_files.begin(), m_files.end(), m_rng);
    rebuild_map();
}

void ImageIndex::sort_by(SortMode mode) {
    m_sort_mode = mode;
    switch (mode) {
    case SortMode::Name:   sort_by_name(); break;
    case SortMode::Date:   sort_by_date(); break;
    case SortMode::Size:   sort_by_size(); break;
    case SortMode::Random: sort_random();  break;
    }
}

void ImageIndex::rebuild_map() {
    m_path_to_idx.clear();
    m_path_to_idx.reserve(m_files.size());
    for (int i = 0; i < static_cast<int>(m_files.size()); ++i) {
        m_path_to_idx[m_files[i].path] = i;
    }
}

// ── Query ────────────────────────────────────────────────────

bool ImageIndex::remove(int idx) {
    if (idx < 0 || idx >= static_cast<int>(m_files.size())) return false;
    m_files.erase(m_files.begin() + idx);
    rebuild_map();
    return true;
}

const std::wstring& ImageIndex::path_at(size_t idx) const {
    if (idx < m_files.size()) return m_files[idx].path;
    return EMPTY_STR;
}

std::wstring ImageIndex::relpath_at(size_t idx) const {
    if (idx >= m_files.size()) return L"";
    const auto& full = m_files[idx].path;
    if (m_root_dir.empty()) return full;
    size_t root_len = m_root_dir.size();
    if (full.size() > root_len &&
        _wcsnicmp(full.c_str(), m_root_dir.c_str(), root_len) == 0) {
        size_t start = root_len;
        if (full[start] == L'\\' || full[start] == L'/') start++;
        return full.substr(start);
    }
    return full;
}

int ImageIndex::index_of(const std::wstring& path) const {
    auto it = m_path_to_idx.find(path);
    return (it != m_path_to_idx.end()) ? it->second : -1;
}

// ── Scan ─────────────────────────────────────────────────────

int ImageIndex::scan(const std::wstring& directory, bool recursive) {
    m_files.clear();
    m_path_to_idx.clear();
    m_root_dir = directory;

    std::error_code error;
    if (!fs::is_directory(directory, error) || error) {
        return -1;
    }

    auto add_entry = [&](const fs::directory_entry& entry) {
        std::error_code entry_error;
        if (!entry.is_regular_file(entry_error) || entry_error) return;

        auto ext = entry.path().extension().wstring();
        for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
        if (!is_image_ext(ext)) return;

        ImageEntry image;
        image.path = entry.path().wstring();
        image.name = entry.path().filename().wstring();
        image.size = entry.file_size(entry_error);
        if (entry_error) {
            entry_error.clear();
            image.size = 0;
        }
        image.mtime = entry.last_write_time(entry_error).time_since_epoch().count();
        if (entry_error) image.mtime = 0;
        m_files.push_back(std::move(image));
    };

    const auto options = fs::directory_options::skip_permission_denied;
    if (recursive) {
        fs::recursive_directory_iterator it(directory, options, error);
        const fs::recursive_directory_iterator end;
        if (error) return -1;
        while (it != end) {
            add_entry(*it);
            it.increment(error);
            if (error) error.clear();
        }
    } else {
        fs::directory_iterator it(directory, options, error);
        const fs::directory_iterator end;
        if (error) return -1;
        while (it != end) {
            add_entry(*it);
            it.increment(error);
            if (error) error.clear();
        }
    }

    // Apply current sort mode (Name by default, or by-path for recursive)
    if (m_sort_mode == SortMode::Name && recursive)
        sort_by_path();
    else
        sort_by(m_sort_mode);

    return static_cast<int>(m_files.size());
}

} // namespace mv
