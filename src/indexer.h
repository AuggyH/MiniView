#pragma once
#include <string>
#include <vector>
#include <unordered_map>

namespace mv {

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
    int  index_of(const std::wstring& path) const;

    /// Remove entry at sorted index. Returns true if removed.
    bool remove(int idx);

private:
    void sort_by_name();
    void rebuild_map();

    std::vector<ImageEntry>                  m_files;
    std::unordered_map<std::wstring, int>   m_path_to_idx;
    std::wstring                             m_root_dir;
};

} // namespace mv
