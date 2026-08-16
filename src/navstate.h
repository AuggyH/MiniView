#pragma once
// Navigation state model for the left collection panel + breadcrumbs
// (Issue #5 P2). Pure, header-only, Windows-free — the testable seam that
// App drives with Win32/file-system side effects (mirrors the
// ComicAppController pattern: App routes, state machine lives here).
//
// Design decisions (NAVIGATION_DESIGN.md, PO-approved 2026-08-07):
// - Single active collection; the panel and the grid breadcrumb are two
//   projections of the same path state (single source of truth).
// - Left panel: grid mode shows by default, image mode hides by default,
//   but once the user toggles (B) the expanded flag sticks and is retained
//   across mode switches (same semantics as the right I panel).
// - Collection switching is generation-based: stale scan results are
//   dropped; a switch never manufactures a selection.
// - Directory tree is lazy: startup enumerates nothing; a node enumerates
//   its children + image count once, on first expand; results are cached.

#include "indexer.h"

#include <algorithm>
#include <cstdint>
#include <cctype>
#include <cwctype>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace mv {

// ── Collection model (NAVIGATION_DESIGN.md §3.1) ─────────────

enum class CollectionSource {
    Directory,   // plain directory collection
    Recursive,   // Ctrl+R recursive result rooted at a directory
    Favorite,    // persisted favorite (P3 — placeholder in this phase)
    Album,       // named album group (P3)
};

struct Collection {
    std::wstring    id;           // stable id (directory = normalized absolute path)
    CollectionSource source = CollectionSource::Directory;
    std::wstring    root;         // root path
    SortMode        sort = SortMode::Name;   // per-collection sort memory (D-12)
    bool            recursive = false;
    std::wstring    display_name;
};

// Case-insensitive, backslash-normalized identity key for a directory path.
// Trailing separators are NOT stripped here; comparisons use
// nav_path_equal / nav_path_is_ancestor which trim them.
inline std::wstring normalize_collection_key(std::wstring path) {
    for (auto& ch : path) {
        if (ch == L'/') ch = L'\\';
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return path;
}

inline std::wstring trim_trailing_separators(std::wstring path) {
    while (path.size() > 1 && path.back() == L'\\') path.pop_back();
    return path;
}

inline bool nav_path_equal(const std::wstring& a, const std::wstring& b) {
    return trim_trailing_separators(normalize_collection_key(a))
        == trim_trailing_separators(normalize_collection_key(b));
}

inline bool nav_path_is_ancestor(
    const std::wstring& ancestor, const std::wstring& path) {
    const std::wstring key =
        trim_trailing_separators(normalize_collection_key(ancestor));
    const std::wstring full =
        trim_trailing_separators(normalize_collection_key(path));
    if (full == key) return true;
    if (full.size() <= key.size()) return false;
    if (full.compare(0, key.size(), key) != 0) return false;
    return full[key.size()] == L'\\';
}

// ── Panel expand/focus state machine ────────────────────────

enum class NavFocusTarget { Main, LeftPanel };
enum class NavPanelTab { Directories, Favorites };

class NavPanelState {
public:
    // Grid mode defaults to visible, image mode to hidden. Until the user
    // toggles once (B key), the mode default is applied automatically.
    // After the first toggle the expanded flag is retained across mode
    // switches (mirrors the right I panel behavior).
    bool visible(bool grid_mode) const noexcept {
        return m_auto_managed ? grid_mode : m_expanded;
    }
    bool expanded() const noexcept { return m_expanded; }
    bool auto_managed() const noexcept { return m_auto_managed; }

    void toggle() noexcept {
        m_auto_managed = false;
        m_expanded = !m_expanded;
        if (!m_expanded) m_focus = NavFocusTarget::Main;
    }

    NavFocusTarget focus() const noexcept { return m_focus; }
    bool focused() const noexcept { return m_focus == NavFocusTarget::LeftPanel; }
    void set_focus(NavFocusTarget target) noexcept { m_focus = target; }
    void release_focus() noexcept { m_focus = NavFocusTarget::Main; }
    bool cycle_focus() noexcept {
        m_focus = (m_focus == NavFocusTarget::Main)
            ? NavFocusTarget::LeftPanel : NavFocusTarget::Main;
        return focused();
    }

    NavPanelTab tab() const noexcept { return m_tab; }
    void set_tab(NavPanelTab tab) noexcept { m_tab = tab; }
    void toggle_tab() noexcept {
        m_tab = (m_tab == NavPanelTab::Directories)
            ? NavPanelTab::Favorites : NavPanelTab::Directories;
    }

private:
    bool m_expanded = true;
    bool m_auto_managed = true;
    NavFocusTarget m_focus = NavFocusTarget::Main;
    NavPanelTab m_tab = NavPanelTab::Directories;
};

// ── Collection switch generation controller ─────────────────
// Async scan with stale-result cancellation (D-13). request() bumps the
// generation and marks a scan in flight; finish(gen) applies only when the
// result belongs to the newest request. invalidate() cancels any in-flight
// scan without starting a new one (e.g. another synchronous entry point).

class NavSwitchController {
public:
    std::uint64_t request() noexcept {
        ++m_generation;
        m_in_flight = true;
        return m_generation;
    }
    void invalidate() noexcept {
        ++m_generation;
        m_in_flight = false;
    }
    bool finish(std::uint64_t generation) noexcept {
        if (generation != m_generation || !m_in_flight) return false;
        m_in_flight = false;
        return true;
    }
    bool in_flight() const noexcept { return m_in_flight; }
    std::uint64_t generation() const noexcept { return m_generation; }

private:
    std::uint64_t m_generation = 0;
    bool m_in_flight = false;
};

// ── Per-collection sort/recursive memory (D-12, P2 state layer) ──

struct CollectionMemory {
    SortMode sort = SortMode::Name;
    bool recursive = false;
};

class CollectionSortMemory {
public:
    CollectionMemory memory_for(const std::wstring& id) const {
        const auto it = m_memory.find(
            trim_trailing_separators(normalize_collection_key(id)));
        return it == m_memory.end() ? CollectionMemory{} : it->second;
    }
    void remember(const std::wstring& id, const CollectionMemory& memory) {
        m_memory[trim_trailing_separators(normalize_collection_key(id))] = memory;
    }
    void forget(const std::wstring& id) {
        m_memory.erase(trim_trailing_separators(normalize_collection_key(id)));
    }
    std::size_t size() const noexcept { return m_memory.size(); }

private:
    std::unordered_map<std::wstring, CollectionMemory> m_memory;
};

// ── Breadcrumb path model ───────────────────────────────────

// Split a directory path into display segments, root first ("D:", then
// components). Handles drive roots, UNC roots and trailing separators.
inline std::vector<std::wstring> split_path_segments(const std::wstring& raw) {
    std::wstring path = raw;
    for (auto& ch : path) {
        if (ch == L'/') ch = L'\\';
    }
    while (path.size() > 1 && path.back() == L'\\') path.pop_back();
    std::vector<std::wstring> segments;
    if (path.empty()) return segments;

    std::size_t start = 0;
    if (path.size() >= 2 && path[1] == L':') {
        segments.push_back(path.substr(0, 2));  // "D:"
        start = 2;
    } else if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') {
        // UNC root: \\server\share
        const std::size_t pos = path.find(L'\\', 2);
        if (pos == std::wstring::npos) {
            segments.push_back(path);
            return segments;
        }
        const std::size_t pos2 = path.find(L'\\', pos + 1);
        if (pos2 == std::wstring::npos) {
            segments.push_back(path);
            return segments;
        }
        segments.push_back(path.substr(0, pos2));
        start = pos2;
    } else if (path[0] == L'\\') {
        segments.push_back(L"\\");
        start = 1;
    }

    while (start < path.size()) {
        if (path[start] == L'\\') {
            ++start;
            continue;
        }
        std::size_t pos = path.find(L'\\', start);
        if (pos == std::wstring::npos) pos = path.size();
        segments.push_back(path.substr(start, pos - start));
        start = pos;
    }
    return segments;
}

// Reconstruct a directory path from segments up to and including `up_to`.
inline std::wstring path_from_segments(
    const std::vector<std::wstring>& segments, int up_to) {
    if (segments.empty() || up_to < 0) return L"";
    up_to = std::min(up_to, static_cast<int>(segments.size()) - 1);
    if (up_to == 0) return segments[0];
    const bool drive_root = segments[0].size() == 2 && segments[0][1] == L':';
    const bool unc_root = segments[0].size() >= 2
        && segments[0][0] == L'\\' && segments[0][1] == L'\\';
    std::wstring out = segments[0];
    if (drive_root || unc_root || segments[0] == L"\\") out += L'\\';
    for (int i = 1; i <= up_to; ++i) {
        if (i > 1) out += L'\\';
        out += segments[i];
    }
    return out;
}

struct NavBreadcrumbItem {
    int segment_index = -1;  // index into source segments; -1 for ellipsis
    bool ellipsis = false;
    float x = 0.0f;
    float width = 0.0f;
};

struct NavBreadcrumbLayout {
    std::vector<NavBreadcrumbItem> items;
    float total_width = 0.0f;
    bool truncated = false;
};

// Layout breadcrumb segments with middle-ellipsis: when everything fits,
// all segments are shown; otherwise the first and last segment stay visible
// and the middle is collapsed into a single "…" slot (NAVIGATION_DESIGN
// §2.2.2). `measure(text)` returns the rendered width of a segment.
template <typename Measure>
inline NavBreadcrumbLayout layout_breadcrumb(
    const std::vector<std::wstring>& segments,
    float max_width, float gap, float ellipsis_width, Measure measure) {
    NavBreadcrumbLayout out;
    if (segments.empty() || max_width <= 0.0f) return out;

    std::vector<float> widths(segments.size());
    float total = 0.0f;
    for (std::size_t i = 0; i < segments.size(); ++i) {
        widths[i] = std::max(0.0f, measure(segments[i]));
        total += widths[i];
    }
    total += gap * static_cast<float>(segments.size() - 1);

    if (total <= max_width || segments.size() <= 1) {
        float x = 0.0f;
        for (std::size_t i = 0; i < segments.size(); ++i) {
            out.items.push_back(
                {static_cast<int>(i), false, x, widths[i]});
            x += widths[i] + gap;
        }
        out.total_width = total;
        return out;
    }

    out.truncated = true;
    const int first = 0;
    const int last = static_cast<int>(segments.size()) - 1;
    std::vector<std::pair<int, float>> slots;  // (segment_index | -1, width)
    const float need_first_last =
        widths[first] + gap + widths[last];
    const float need_with_ellipsis =
        need_first_last + gap + ellipsis_width;
    if (need_with_ellipsis <= max_width) {
        slots.push_back({first, widths[first]});
        slots.push_back({-1, ellipsis_width});
        slots.push_back({last, widths[last]});
    } else if (need_first_last <= max_width) {
        slots.push_back({first, widths[first]});
        slots.push_back({last, widths[last]});
    } else if (widths[last] <= max_width) {
        slots.push_back({last, widths[last]});
    } else {
        slots.push_back({first, widths[first]});
    }

    float x = 0.0f;
    for (const auto& [index, width] : slots) {
        out.items.push_back({index, index < 0, x, width});
        x += width + gap;
    }
    out.total_width = std::max(0.0f, x - gap);
    return out;
}

// Hit-test a breadcrumb click; returns the item index (for hover highlight)
// or -1. Ellipsis slots never hit.
inline int breadcrumb_hit_item(
    const NavBreadcrumbLayout& layout, float click_x) {
    for (int i = 0; i < static_cast<int>(layout.items.size()); ++i) {
        const auto& item = layout.items[static_cast<size_t>(i)];
        if (item.ellipsis) continue;
        if (click_x >= item.x && click_x < item.x + item.width) return i;
    }
    return -1;
}

// Hit-test a breadcrumb click; returns the source segment index or -1.
inline int breadcrumb_hit_segment(
    const NavBreadcrumbLayout& layout, float click_x) {
    const int item = breadcrumb_hit_item(layout, click_x);
    if (item < 0) return -1;
    return layout.items[static_cast<size_t>(item)].segment_index;
}

// ── Lazy directory tree model ───────────────────────────────

enum class NavNodeState { Collapsed, Expanded, Loading, Error };

struct NavTreeNode {
    std::uint64_t id = 0;
    std::uint64_t parent = 0;
    std::wstring path;
    std::wstring name;
    int depth = 0;
    std::vector<std::uint64_t> children;  // sorted by name
    NavNodeState state = NavNodeState::Collapsed;
    bool enumerated = false;  // children loaded at least once
    int image_count = -1;     // lazy count; -1 = not counted yet
    std::wstring error;       // non-empty when state == Error
    bool is_root = false;
    std::uint64_t load_generation = 0;  // in-flight async expansion id
};

struct NavChildInfo {
    std::wstring path;
    std::wstring name;
};

struct NavTreeRow {
    std::uint64_t node_id = 0;
    std::wstring path;
    std::wstring name;
    int depth = 0;
    float y = 0.0f;
    float height = 0.0f;
    bool expandable = false;   // has children (enumerated) or enumeration pending
    bool expanded = false;
    bool loading = false;
    bool error = false;
    std::wstring error_text;  // Chinese error message when state == Error
    bool is_root = false;
    int image_count = -1;      // -1 = not counted yet
    std::wstring badge;        // e.g. L"[递归]" — set by the view layer
    bool highlighted = false;  // row is the active collection node
};

class NavTreeModel {
public:
    void clear() {
        m_nodes.clear();
        m_roots.clear();
    }
    std::size_t size() const noexcept { return m_nodes.size(); }
    const NavTreeNode* node(std::uint64_t id) const {
        const auto it = m_nodes.find(id);
        return it == m_nodes.end() ? nullptr : &it->second;
    }
    const std::vector<std::uint64_t>& roots() const noexcept {
        return m_roots;
    }

    // Add the drive/favorites root. Returns the node id (existing if the
    // same path is already rooted).
    std::uint64_t add_root(const std::wstring& path, const std::wstring& name) {
        const std::wstring key = normalize_collection_key(path);
        for (const auto id : m_roots) {
            const auto& root = m_nodes.at(id);
            if (normalize_collection_key(root.path) == key) return id;
        }
        NavTreeNode root;
        root.id = ++m_next_id;
        root.path = path;
        root.name = name.empty() ? path : name;
        root.depth = 0;
        root.is_root = true;
        root.state = NavNodeState::Collapsed;
        m_nodes.emplace(root.id, std::move(root));
        m_roots.push_back(root.id);
        return root.id;
    }

    // Expand a collapsed node. Returns the generation to hand to the async
    // enumerator, or 0 when there is nothing to enumerate (already expanded
    // / loading / cached from a previous expansion / unknown id).
    std::uint64_t request_expand(std::uint64_t id) {
        auto it = m_nodes.find(id);
        if (it == m_nodes.end()) return 0;
        NavTreeNode& n = it->second;
        if (n.enumerated) {
            n.state = NavNodeState::Expanded;  // instant re-expand from cache
            return 0;
        }
        if (n.state == NavNodeState::Expanded) return 0;
        if (n.state == NavNodeState::Loading) return 0;
        n.state = NavNodeState::Loading;
        n.load_generation = ++m_expand_generation;
        return n.load_generation;
    }

    // Apply an async enumeration result. Returns false when the result is
    // stale (generation mismatch), the node is not loading, or the id is
    // unknown. Children are sorted by name; the image count is cached.
    // `cancelled` drops a replaced queued job back to Collapsed silently.
    bool finish_expand(
        std::uint64_t id, std::uint64_t generation,
        const std::vector<NavChildInfo>& children, int image_count,
        bool ok, const std::wstring& error, bool cancelled = false) {
        auto it = m_nodes.find(id);
        if (it == m_nodes.end()) return false;
        NavTreeNode& n = it->second;
        if (n.state != NavNodeState::Loading) return false;
        if (n.load_generation != generation) return false;
        n.load_generation = 0;
        if (cancelled) {
            n.state = NavNodeState::Collapsed;
            return true;
        }
        n.image_count = image_count;
        if (!ok) {
            n.state = NavNodeState::Error;
            n.error = error;
            return true;
        }
        n.state = NavNodeState::Expanded;
        n.enumerated = true;
        n.error.clear();
        n.children.clear();
        n.children.reserve(children.size());
        for (const auto& child : children) {
            NavTreeNode child_node;
            child_node.id = ++m_next_id;
            child_node.parent = id;
            child_node.path = child.path;
            child_node.name = child.name;
            child_node.depth = n.depth + 1;
            n.children.push_back(child_node.id);
            m_nodes.emplace(child_node.id, std::move(child_node));
        }
        std::sort(n.children.begin(), n.children.end(),
            [this](std::uint64_t left, std::uint64_t right) {
                return _wcsicmp_nav(m_nodes.at(left).name, m_nodes.at(right).name) < 0;
            });
        return true;
    }

    void collapse(std::uint64_t id) {
        auto it = m_nodes.find(id);
        if (it == m_nodes.end() || it->second.state == NavNodeState::Collapsed)
            return;
        it->second.state = NavNodeState::Collapsed;  // children stay cached
    }

    // Deepest known node that is an ancestor-or-equal of `path` (0 = none).
    std::uint64_t find_ancestor(const std::wstring& path) const {
        if (path.empty() || m_nodes.empty()) return 0;
        std::uint64_t current = 0;
        for (const auto root_id : m_roots) {
            const auto& root = m_nodes.at(root_id);
            if (nav_path_is_ancestor(root.path, path)) {
                current = root_id;
                break;
            }
        }
        while (current != 0) {
            const auto& node = m_nodes.at(current);
            if (nav_path_equal(node.path, path)) return current;
            std::uint64_t next = 0;
            for (const auto child_id : node.children) {
                const auto& child = m_nodes.at(child_id);
                if (nav_path_is_ancestor(child.path, path)) {
                    next = child_id;
                    break;
                }
            }
            if (next == 0) return current;
            current = next;
        }
        return current;
    }

    // Reveal plan for `path`: nodes to enumerate (in order) so the tree can
    // show the path, plus the deepest node already known (highlight target).
    struct RevealPlan {
        std::vector<std::uint64_t> expansions;
        std::uint64_t highlight_id = 0;
    };

    RevealPlan reveal(const std::wstring& path) const {
        RevealPlan plan;
        if (path.empty() || m_nodes.empty()) return plan;
        std::uint64_t current = 0;
        for (const auto root_id : m_roots) {
            const auto& root = m_nodes.at(root_id);
            if (nav_path_is_ancestor(root.path, path)) {
                current = root_id;
                break;
            }
        }
        if (current == 0) return plan;
        plan.highlight_id = current;
        for (;;) {
            const auto& node = m_nodes.at(current);
            if (nav_path_equal(node.path, path)) {
                plan.highlight_id = current;
                break;
            }
            std::uint64_t next = 0;
            for (const auto child_id : node.children) {
                const auto& child = m_nodes.at(child_id);
                if (nav_path_is_ancestor(child.path, path)) {
                    next = child_id;
                    break;
                }
            }
            if (next == 0) break;
            if (nav_path_equal(m_nodes.at(next).path, path)) {
                plan.highlight_id = next;
                break;
            }
            if (m_nodes.at(next).state != NavNodeState::Expanded) {
                plan.expansions.push_back(next);
                break;
            }
            current = next;
            plan.highlight_id = current;
        }
        return plan;
    }

    // DFS-order focus navigation over the visible rows (children of a
    // collapsed node are not visited). Returns 0 when there is no row.
    std::uint64_t focus_next(std::uint64_t current) const {
        const std::vector<std::uint64_t> order = flatten_order();
        if (order.empty()) return 0;
        auto it = std::find(order.begin(), order.end(), current);
        if (it == order.end()) return order.front();
        ++it;
        return it == order.end() ? order.back() : *it;
    }

    std::uint64_t focus_prev(std::uint64_t current) const {
        const std::vector<std::uint64_t> order = flatten_order();
        if (order.empty()) return 0;
        auto it = std::find(order.begin(), order.end(), current);
        if (it == order.end()) return order.front();
        if (it == order.begin()) return order.front();
        --it;
        return *it;
    }

    // Total visible row count (all roots' expanded subtrees).
    std::size_t visible_row_count() const noexcept {
        std::size_t count = 0;
        for (const auto root_id : m_roots)
            count += subtree_visible_count(root_id);
        return count;
    }

    // Project the tree onto a viewport: depth-first visible rows whose
    // [y, y+row_height) intersects [scroll_y, scroll_y + viewport_height],
    // with `overscan` rows of padding on each side.
    std::vector<NavTreeRow> layout_rows(
        float scroll_y, float viewport_height, float row_height,
        float overscan = 1.0f) const {
        std::vector<NavTreeRow> rows;
        const float top = std::max(0.0f, scroll_y - overscan * row_height);
        const float bottom = scroll_y + viewport_height
            + overscan * row_height;
        float y = 0.0f;
        for (const auto root_id : m_roots)
            collect_rows(root_id, y, row_height, top, bottom, rows);
        return rows;
    }

    // Total content height for the tree scrollbar.
    float content_height(float row_height) const noexcept {
        return static_cast<float>(visible_row_count()) * row_height;
    }

private:
    static int _wcsicmp_nav(const std::wstring& left, const std::wstring& right) {
        std::size_t n = std::min(left.size(), right.size());
        for (std::size_t i = 0; i < n; ++i) {
            const wchar_t a = static_cast<wchar_t>(std::towlower(left[i]));
            const wchar_t b = static_cast<wchar_t>(std::towlower(right[i]));
            if (a != b) return a < b ? -1 : 1;
        }
        if (left.size() == right.size()) return 0;
        return left.size() < right.size() ? -1 : 1;
    }

    std::size_t subtree_visible_count(std::uint64_t id) const noexcept {
        const auto& node = m_nodes.at(id);
        std::size_t count = 1;
        if (node.state != NavNodeState::Expanded) return count;
        for (const auto child_id : node.children)
            count += subtree_visible_count(child_id);
        return count;
    }

    void collect_rows(std::uint64_t id, float& y, float row_height,
        float top, float bottom, std::vector<NavTreeRow>& rows) const {
        const auto& node = m_nodes.at(id);
        if (y + row_height >= top && y < bottom) {
            NavTreeRow row;
            row.node_id = id;
            row.path = node.path;
            row.name = node.name;
            row.depth = node.depth;
            row.y = y;
            row.height = row_height;
            row.is_root = node.is_root;
            row.image_count = node.image_count;
            row.expandable = !node.children.empty()
                || node.state == NavNodeState::Loading
                || node.state == NavNodeState::Error
                || (node.state == NavNodeState::Collapsed && !node.enumerated);
            row.expanded = node.state == NavNodeState::Expanded;
            row.loading = node.state == NavNodeState::Loading;
            row.error = node.state == NavNodeState::Error;
            row.error_text = node.error;
            rows.push_back(std::move(row));
        }
        y += row_height;
        if (node.state != NavNodeState::Expanded) return;
        for (const auto child_id : node.children)
            collect_rows(child_id, y, row_height, top, bottom, rows);
    }

    std::vector<std::uint64_t> flatten_order() const {
        std::vector<std::uint64_t> order;
        for (const auto root_id : m_roots)
            flatten_subtree(root_id, order);
        return order;
    }

    void flatten_subtree(
        std::uint64_t id, std::vector<std::uint64_t>& order) const {
        const auto& node = m_nodes.at(id);
        order.push_back(id);
        if (node.state != NavNodeState::Expanded) return;
        for (const auto child_id : node.children)
            flatten_subtree(child_id, order);
    }

    std::unordered_map<std::uint64_t, NavTreeNode> m_nodes;
    std::vector<std::uint64_t> m_roots;
    std::uint64_t m_next_id = 0;
    std::uint64_t m_expand_generation = 0;
};

// Click-zone classification inside a tree row: the leading arrow toggles
// expand/collapse; the rest of the row switches the collection.
enum class NavTreeRowZone { None, Arrow, Body };

inline NavTreeRowZone hit_nav_tree_row(
    float row_left, float click_x, int depth,
    float indent, float arrow_width) {
    const float arrow_start = row_left + indent * static_cast<float>(depth);
    const float arrow_end = arrow_start + arrow_width;
    if (click_x >= arrow_start && click_x < arrow_end) {
        return NavTreeRowZone::Arrow;
    }
    if (click_x >= arrow_end) return NavTreeRowZone::Body;
    return NavTreeRowZone::None;
}

// ── Collection switch apply planning ────────────────────────
// Pure decision helpers for applying an async scan result. The App commits
// the index and grid state afterwards; these keep the rules testable.

enum class CollectionApplyAction {
    None,             // nothing to do (invalid)
    EnterGrid,        // switch completed → enter grid (from image mode)
    RefreshGrid,      // switch completed → rebuild the visible grid
    ShowOpenError,    // scan failed → keep old collection, show feedback
};

struct CollectionApplyInput {
    int scan_result = -1;   // ImageIndex::scan() result (< 0 = failure)
    bool grid_mode = false;
};

inline CollectionApplyAction plan_collection_apply(
    const CollectionApplyInput& input) {
    if (input.scan_result < 0) return CollectionApplyAction::ShowOpenError;
    return input.grid_mode
        ? CollectionApplyAction::RefreshGrid
        : CollectionApplyAction::EnterGrid;
}

// Reset selection state on collection switch. A fresh collection must never
// inherit the old collection's selection (NAVIGATION_DESIGN §2.5: switching
// collections never defaults to the first image).
inline void reset_collection_selection(
    int& current_index, int& grid_selection, int& grid_saved_index,
    std::vector<bool>& selected, int& selection_anchor) {
    current_index = -1;
    grid_selection = -1;
    grid_saved_index = -1;
    selected.clear();
    selection_anchor = -1;
}

} // namespace mv
