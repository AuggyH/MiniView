#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
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

/// One scan root of a multi-root collection (album). Each root carries its
/// own recursive flag; entries are merged and deduplicated by path.
struct ScanRoot {
    std::wstring path;
    bool recursive = false;
};

class ImageIndex {
public:
    int  scan(const std::wstring& directory, bool recursive = true);
    void clear();

    /// Scan multiple roots into one index (albums). Unreadable roots are
    /// skipped; duplicate paths (e.g. a parent root that is recursive and a
    /// child root) are merged. Returns the total entry count.
    int  scan_many(const std::vector<ScanRoot>& roots);

    /// Build the index from explicit image paths (the fixed favourite
    /// album). Missing paths are skipped; entries are sorted by the current
    /// sort mode. Returns the entry count.
    int  load_paths(const std::vector<std::wstring>& paths);

    size_t size() const { return m_files.size(); }
    bool   empty() const { return m_files.empty(); }
    const std::wstring& directory() const { return m_root_dir; }

    const std::wstring& path_at(size_t idx) const;
    std::wstring        relpath_at(size_t idx) const;
    int  index_of(const std::wstring& path) const;

    /// Remove entry at sorted index. Returns true if removed.
    bool remove(int idx);

    /// Remove all valid sorted indices with one compaction and map rebuild.
    size_t remove_many(const std::vector<int>& indices);

    /// Re-sort existing entries by the given mode.
    void sort_by(SortMode mode);

    /// Get current sort mode.
    SortMode sort_mode() const { return m_sort_mode; }

    /// True for the image extensions MinView indexes (lowercase, includes dot).
    static bool is_supported_image_extension(const std::wstring& ext);

private:
    void sort_by_name();
    void sort_by_path();
    void sort_by_date();
    void sort_by_size();
    void sort_random();
    void rebuild_map();
    // Append one directory's images into m_files (no clear, no sort).
    // Existing normalized keys in seen are skipped (cross-root dedupe).
    int scan_directory(const std::wstring& directory, bool recursive,
        std::unordered_set<std::wstring>& seen);

    std::vector<ImageEntry>                  m_files;
    std::unordered_map<std::wstring, int>   m_path_to_idx;
    std::wstring                             m_root_dir;
    SortMode                                 m_sort_mode = SortMode::Name;
    std::mt19937                             m_rng{std::random_device{}()};
};

} // namespace mv
