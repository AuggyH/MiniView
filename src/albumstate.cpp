#include "albumstate.h"

#include <Windows.h>
#include <cwctype>
#include <utility>

namespace mv {

// ── Operations ────────────────────────────────────────────────────────────

bool add_album(AlbumStore& store, const std::wstring& name) {
    if (album_name_blank(name)) return false;
    for (const auto& album : store.albums)
        if (_wcsicmp(album.name.c_str(), name.c_str()) == 0) return false;
    store.albums.push_back(Album{name, {}});
    return true;
}

bool rename_album(AlbumStore& store, std::size_t index,
                  const std::wstring& name) {
    if (index >= store.albums.size() || album_name_blank(name)) return false;
    for (std::size_t i = 0; i < store.albums.size(); ++i)
        if (i != index
            && _wcsicmp(store.albums[i].name.c_str(), name.c_str()) == 0)
            return false;
    store.albums[index].name = name;
    return true;
}

bool remove_album(AlbumStore& store, std::size_t index) {
    if (index >= store.albums.size()) return false;
    store.albums.erase(store.albums.begin()
        + static_cast<std::ptrdiff_t>(index));
    return true;
}

bool add_folder(AlbumStore& store, std::size_t album, std::wstring path,
                bool recursive) {
    if (album >= store.albums.size()) return false;
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    if (path.empty()) return false;
    auto& folders = store.albums[album].folders;
    for (const auto& folder : folders)
        if (_wcsicmp(folder.path.c_str(), path.c_str()) == 0) return false;
    folders.push_back(AlbumFolder{std::move(path), recursive});
    return true;
}

bool remove_folder(AlbumStore& store, std::size_t album, std::size_t folder) {
    if (album >= store.albums.size()) return false;
    auto& folders = store.albums[album].folders;
    if (folder >= folders.size()) return false;
    folders.erase(folders.begin() + static_cast<std::ptrdiff_t>(folder));
    return true;
}

bool move_folder(AlbumStore& store, std::size_t album, std::size_t from,
                 std::size_t to) {
    if (album >= store.albums.size()) return false;
    auto& folders = store.albums[album].folders;
    if (from >= folders.size() || to >= folders.size() || from == to)
        return false;
    AlbumFolder item = std::move(folders[from]);
    folders.erase(folders.begin() + static_cast<std::ptrdiff_t>(from));
    folders.insert(folders.begin() + static_cast<std::ptrdiff_t>(to),
                   std::move(item));
    return true;
}

bool set_folder_recursive(AlbumStore& store, std::size_t album,
                          std::size_t folder, bool recursive) {
    if (album >= store.albums.size()) return false;
    auto& folders = store.albums[album].folders;
    if (folder >= folders.size()) return false;
    folders[folder].recursive = recursive;
    return true;
}

bool add_favourite(AlbumStore& store, const std::wstring& path) {
    if (path.empty()) return false;
    for (const auto& existing : store.favourites)
        if (_wcsicmp(existing.c_str(), path.c_str()) == 0) return false;
    store.favourites.push_back(path);
    return true;
}

bool remove_favourite(AlbumStore& store, const std::wstring& path) {
    for (std::size_t i = 0; i < store.favourites.size(); ++i) {
        if (_wcsicmp(store.favourites[i].c_str(), path.c_str()) == 0) {
            store.favourites.erase(store.favourites.begin()
                + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    }
    return false;
}

bool is_favourite(const AlbumStore& store, const std::wstring& path) {
    for (const auto& existing : store.favourites)
        if (_wcsicmp(existing.c_str(), path.c_str()) == 0) return true;
    return false;
}

// ── UTF-8 helpers ─────────────────────────────────────────────────────────

std::string wide_to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
        static_cast<int>(text.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::optional<std::wstring> utf8_to_wide(const std::string& text) {
    if (text.empty()) return std::wstring{};
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return std::nullopt;
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.c_str(),
        static_cast<int>(text.size()), out.data(), needed);
    return out;
}

// ── JSON serialization ────────────────────────────────────────────────────

namespace {

void append_json_string(std::string& out, const std::string& utf8) {
    out.push_back('"');
    for (unsigned char c : utf8) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        default:
            if (c < 0x20) {
                static constexpr char hex[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(hex[c >> 4]);
                out.push_back(hex[c & 0xF]);
            } else {
                out.push_back(static_cast<char>(c));
            }
        }
    }
    out.push_back('"');
}

} // namespace

std::string serialize_album_store(const AlbumStore& store) {
    std::string out;
    out += "{\n";
    out += "  \"version\": 1,\n";
    out += "  \"folder_view\": \""
        + std::string(store.folder_view == AlbumFolderView::Tree
            ? "tree" : "icons") + "\",\n";
    out += "  \"favourites\": [";
    for (std::size_t i = 0; i < store.favourites.size(); ++i) {
        if (i) out.push_back(',');
        out.push_back('\n');
        out += "    ";
        append_json_string(out, wide_to_utf8(store.favourites[i]));
    }
    if (!store.favourites.empty()) out.push_back('\n');
    out += "  ],\n";
    out += "  \"albums\": [";
    for (std::size_t a = 0; a < store.albums.size(); ++a) {
        if (a) out.push_back(',');
        out += "\n    {\n      \"name\": ";
        append_json_string(out, wide_to_utf8(store.albums[a].name));
        out += ",\n      \"folders\": [";
        const auto& folders = store.albums[a].folders;
        for (std::size_t f = 0; f < folders.size(); ++f) {
            if (f) out.push_back(',');
            out += "\n        {\"path\": ";
            append_json_string(out, wide_to_utf8(folders[f].path));
            out += ", \"recursive\": ";
            out += folders[f].recursive ? "true" : "false";
            out += "}";
        }
        if (!folders.empty()) out.push_back('\n');
        out += "      ]\n    }";
    }
    if (!store.albums.empty()) out.push_back('\n');
    out += "  ]\n}\n";
    return out;
}

// ── JSON parsing (tolerant, targeted at the album schema) ────────────────

namespace {

class JsonCursor {
public:
    explicit JsonCursor(const std::string& text) : m_text(text) {}

    void skip_ws() {
        while (m_pos < m_text.size()
            && (m_text[m_pos] == ' ' || m_text[m_pos] == '\t'
                || m_text[m_pos] == '\n' || m_text[m_pos] == '\r'))
            ++m_pos;
    }
    bool expect(char c) {
        skip_ws();
        if (m_pos >= m_text.size() || m_text[m_pos] != c) return false;
        ++m_pos;
        return true;
    }
    bool peek(char c) {
        skip_ws();
        return m_pos < m_text.size() && m_text[m_pos] == c;
    }
    bool at_end() {
        skip_ws();
        return m_pos >= m_text.size();
    }

    // Raw UTF-8 string value (escape-decoded); nullopt on any error.
    std::optional<std::string> parse_string() {
        skip_ws();
        if (m_pos >= m_text.size() || m_text[m_pos] != '"') return std::nullopt;
        ++m_pos;
        std::string out;
        while (m_pos < m_text.size()) {
            const unsigned char c = static_cast<unsigned char>(m_text[m_pos]);
            if (c == '"') {
                ++m_pos;
                return out;
            }
            if (c == '\\') {
                ++m_pos;
                if (m_pos >= m_text.size()) return std::nullopt;
                const char esc = m_text[m_pos++];
                switch (esc) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u': {
                    if (m_pos + 4 > m_text.size()) return std::nullopt;
                    unsigned code = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char h = m_text[m_pos++];
                        unsigned v = 0;
                        if (h >= '0' && h <= '9') v = static_cast<unsigned>(h - '0');
                        else if (h >= 'a' && h <= 'f') v = static_cast<unsigned>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v = static_cast<unsigned>(h - 'A' + 10);
                        else return std::nullopt;
                        code = (code << 4) | v;
                    }
                    // unicode escape → UTF-8 (surrogate pairs combined).
                    wchar_t units[2] = {};
                    int unit_count = 0;
                    if (code >= 0xD800 && code <= 0xDBFF
                        && m_pos + 6 <= m_text.size()
                        && m_text[m_pos] == '\\'
                        && m_text[m_pos + 1] == 'u') {
                        unsigned low = 0;
                        for (int i = 2; i < 6; ++i) {
                            const char h = m_text[m_pos + i];
                            unsigned v = 0;
                            if (h >= '0' && h <= '9') v = static_cast<unsigned>(h - '0');
                            else if (h >= 'a' && h <= 'f') v = static_cast<unsigned>(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') v = static_cast<unsigned>(h - 'A' + 10);
                            else return std::nullopt;
                            low = (low << 4) | v;
                        }
                        if (low >= 0xDC00 && low <= 0xDFFF) {
                            m_pos += 6;
                            units[0] = static_cast<wchar_t>(code);
                            units[1] = static_cast<wchar_t>(low);
                            unit_count = 2;
                        }
                    }
                    if (unit_count == 0) {
                        units[0] = static_cast<wchar_t>(code);
                        unit_count = 1;
                    }
                    const std::wstring wide(units,
                        static_cast<std::size_t>(unit_count));
                    out += wide_to_utf8(wide);
                    break;
                }
                default:
                    return std::nullopt;
                }
                continue;
            }
            out.push_back(static_cast<char>(c));
            ++m_pos;
        }
        return std::nullopt;  // unterminated
    }

    std::optional<bool> parse_bool() {
        skip_ws();
        if (m_pos + 4 <= m_text.size()
            && m_text.compare(m_pos, 4, "true") == 0) {
            m_pos += 4;
            return true;
        }
        if (m_pos + 5 <= m_text.size()
            && m_text.compare(m_pos, 5, "false") == 0) {
            m_pos += 5;
            return false;
        }
        return std::nullopt;
    }

private:
    const std::string& m_text;
    std::size_t m_pos = 0;
};

} // namespace

std::optional<AlbumStore> parse_album_store(const std::string& json) {
    JsonCursor cursor(json);
    if (!cursor.expect('{')) return std::nullopt;
    AlbumStore store;
    int version = 1;
    if (cursor.peek('}')) {
        if (!cursor.expect('}') || !cursor.at_end()) return std::nullopt;
        return store;
    }
    while (true) {
        const auto key = cursor.parse_string();
        if (!key || !cursor.expect(':')) return std::nullopt;
        if (*key == "version") {
            cursor.skip_ws();
            if (!cursor.peek('1')) return std::nullopt;
            cursor.expect('1');
            version = 1;
        } else if (*key == "folder_view") {
            const auto value = cursor.parse_string();
            if (!value) return std::nullopt;
            if (*value == "icons") store.folder_view = AlbumFolderView::Icons;
            else store.folder_view = AlbumFolderView::Tree;
        } else if (*key == "favourites") {
            if (!cursor.expect('[')) return std::nullopt;
            if (!cursor.peek(']')) {
                while (true) {
                    const auto value = cursor.parse_string();
                    if (!value) return std::nullopt;
                    const auto wide = utf8_to_wide(*value);
                    if (!wide) return std::nullopt;
                    store.favourites.push_back(*wide);
                    if (cursor.peek(',')) {
                        cursor.expect(',');
                        continue;
                    }
                    break;
                }
            }
            if (!cursor.expect(']')) return std::nullopt;
        } else if (*key == "albums") {
            if (!cursor.expect('[')) return std::nullopt;
            if (!cursor.peek(']')) {
                while (true) {
                    if (!cursor.expect('{')) return std::nullopt;
                    Album album;
                    if (cursor.peek('}')) {
                        cursor.expect('}');
                    } else {
                        while (true) {
                            const auto album_key = cursor.parse_string();
                            if (!album_key || !cursor.expect(':')) return std::nullopt;
                            if (*album_key == "name") {
                                const auto value = cursor.parse_string();
                                if (!value) return std::nullopt;
                                const auto wide = utf8_to_wide(*value);
                                if (!wide) return std::nullopt;
                                album.name = *wide;
                            } else if (*album_key == "folders") {
                                if (!cursor.expect('[')) return std::nullopt;
                                if (!cursor.peek(']')) {
                                    while (true) {
                                        if (!cursor.expect('{')) return std::nullopt;
                                        AlbumFolder folder;
                                        if (cursor.peek('}')) {
                                            cursor.expect('}');
                                        } else {
                                            while (true) {
                                                const auto folder_key = cursor.parse_string();
                                                if (!folder_key || !cursor.expect(':')) return std::nullopt;
                                                if (*folder_key == "path") {
                                                    const auto value = cursor.parse_string();
                                                    if (!value) return std::nullopt;
                                                    const auto wide = utf8_to_wide(*value);
                                                    if (!wide) return std::nullopt;
                                                    folder.path = *wide;
                                                } else if (*folder_key == "recursive") {
                                                    const auto value = cursor.parse_bool();
                                                    if (!value) return std::nullopt;
                                                    folder.recursive = *value;
                                                } else {
                                                    return std::nullopt;
                                                }
                                                if (cursor.peek(',')) {
                                                    cursor.expect(',');
                                                    continue;
                                                }
                                                break;
                                            }
                                        }
                                        if (!cursor.expect('}')) return std::nullopt;
                                        album.folders.push_back(std::move(folder));
                                        if (cursor.peek(',')) {
                                            cursor.expect(',');
                                            continue;
                                        }
                                        break;
                                    }
                                }
                                if (!cursor.expect(']')) return std::nullopt;
                            } else {
                                return std::nullopt;
                            }
                            if (cursor.peek(',')) {
                                cursor.expect(',');
                                continue;
                            }
                            break;
                        }
                    }
                    if (!cursor.expect('}')) return std::nullopt;
                    store.albums.push_back(std::move(album));
                    if (cursor.peek(',')) {
                        cursor.expect(',');
                        continue;
                    }
                    break;
                }
            }
            if (!cursor.expect(']')) return std::nullopt;
        } else {
            return std::nullopt;  // unknown top-level key — refuse
        }
        if (cursor.peek(',')) {
            cursor.expect(',');
            continue;
        }
        break;
    }
    if (!cursor.expect('}') || !cursor.at_end()) return std::nullopt;
    if (version != 1) return std::nullopt;
    return store;
}

} // namespace mv
