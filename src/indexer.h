#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <random>

namespace mv {

enum class SortMode {
    Name,     // filename, case-insensitive
    Date,     // last-modified, newest first
    Size,     // file size, largest first
    Random,   // Fisher-Yates shuffle
};

struct ImageEntry {
    std::wstring path;
    std::wstring name;
    uint64_t     size = 0;
    uint64_t     mtime = 0;
};

class ImageIndex {
public:
    int  scan(const std::wstring& directory, bool recursive = true);
    void clear();

    size_t size() const { return m_files.size(); }
    bool   empty() const { return m_files.empty(); }
    const std::wstring& directory() const { return m_root_dir; }

    const std::wstring& path_at(size_t idx) const;
    std::wstring        relpath_at(size_t idx) const;
    int  index_of(const std::wstring& path) const;

    /// Remove entry at sorted index. Returns true if removed.
    bool remove(int idx);

    /// Re-sort existing entries by the given mode.
    void sort_by(SortMode mode);

    /// Get current sort mode.
    SortMode sort_mode() const { return m_sort_mode; }

private:
    void sort_by_name();
    void sort_by_path();
    void sort_by_date();
    void sort_by_size();
    void sort_random();
    void rebuild_map();

    std::vector<ImageEntry>                  m_files;
    std::unordered_map<std::wstring, int>   m_path_to_idx;
    std::wstring                             m_root_dir;
    SortMode                                 m_sort_mode = SortMode::Name;
    std::mt19937                             m_rng{std::random_device{}()};
};

} // namespace mv
