#pragma once
// Album / Favourite model (Issue #5 P3).
//
// Pure state + persistence, no UI: albums are named containers of folder
// references (each folder carries its own recursive flag); the fixed
// "favourite" album holds single-image paths. Removing a folder from an
// album never touches the local files. JSON is hand-parsed with tolerance:
// any structural error yields nullopt and the caller keeps working.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mv {

enum class AlbumFolderView { Tree, Icons };

struct AlbumFolder {
    std::wstring path;
    bool recursive = false;
};

struct Album {
    std::wstring name;
    std::vector<AlbumFolder> folders;
};

struct AlbumStore {
    std::vector<std::wstring> favourites;   // fixed 收藏 album: single images
    std::vector<Album> albums;              // user albums (folder containers)
    AlbumFolderView folder_view = AlbumFolderView::Tree;  // left-panel mode
};

// ── Operations (pure; invalid indices fail without exceptions) ────────────

inline bool album_name_blank(const std::wstring& name) {
    for (wchar_t c : name)
        if (c != L' ' && c != L'\t') return false;
    return true;
}

bool add_album(AlbumStore& store, const std::wstring& name);
bool rename_album(AlbumStore& store, std::size_t index,
                  const std::wstring& name);
bool remove_album(AlbumStore& store, std::size_t index);
bool add_folder(AlbumStore& store, std::size_t album, std::wstring path,
                bool recursive);
bool remove_folder(AlbumStore& store, std::size_t album, std::size_t folder);
bool move_folder(AlbumStore& store, std::size_t album, std::size_t from,
                 std::size_t to);
bool set_folder_recursive(AlbumStore& store, std::size_t album,
                          std::size_t folder, bool recursive);
bool add_favourite(AlbumStore& store, const std::wstring& path);
bool remove_favourite(AlbumStore& store, const std::wstring& path);
bool is_favourite(const AlbumStore& store, const std::wstring& path);

// ── Persistence ───────────────────────────────────────────────────────────

// Tolerant parser: any structural error or wrong type → nullopt. Unknown
// keys are skipped; missing optional fields take defaults.
std::optional<AlbumStore> parse_album_store(const std::string& utf8_json);
std::string serialize_album_store(const AlbumStore& store);

// ── UTF-8 helpers (paths cross the JSON boundary as UTF-8) ────────────────

std::string wide_to_utf8(const std::wstring& text);
std::optional<std::wstring> utf8_to_wide(const std::string& text);

} // namespace mv
