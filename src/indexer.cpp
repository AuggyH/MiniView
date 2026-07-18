#include "indexer.h"
#include <filesystem>
#include <algorithm>

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

void ImageIndex::sort_by_name() {
    std::sort(m_files.begin(), m_files.end(),
        [](const ImageEntry& a, const ImageEntry& b) {
            auto na = a.name, nb = b.name;
            for (auto& c : na) c = towlower(c);
            for (auto& c : nb) c = towlower(c);
            return na < nb;
        });
    rebuild_map();
}

void ImageIndex::rebuild_map() {
    m_path_to_idx.clear();
    for (int i = 0; i < static_cast<int>(m_files.size()); ++i) {
        m_path_to_idx[m_files[i].path] = i;
    }
}

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

int ImageIndex::index_of(const std::wstring& path) const {
    auto it = m_path_to_idx.find(path);
    return (it != m_path_to_idx.end()) ? it->second : -1;
}

int ImageIndex::scan(const std::wstring& directory, bool recursive) {
    m_files.clear();
    m_path_to_idx.clear();
    m_root_dir = directory;

    try {
        auto scan_dir = [&](const auto& it) {
            for (auto& entry : it) {
                if (!entry.is_regular_file()) continue;
                auto ext = entry.path().extension().wstring();
                for (auto& c : ext) c = towlower(c);
                if (!is_image_ext(ext)) continue;

                ImageEntry ie;
                ie.path  = entry.path().wstring();
                ie.name  = entry.path().filename().wstring();
                ie.size  = entry.file_size();
                ie.mtime = entry.last_write_time().time_since_epoch().count();
                m_files.push_back(std::move(ie));
            }
        };

        if (recursive) {
            scan_dir(fs::recursive_directory_iterator(directory));
        } else {
            scan_dir(fs::directory_iterator(directory));
        }
    } catch (...) {
        return -1;
    }

    sort_by_name();
    return static_cast<int>(m_files.size());
}

} // namespace mv
