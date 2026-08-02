#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mv {

constexpr float kComicDefaultViewportRatio = 0.92f;
constexpr float kComicDefaultMaxWidthDip = 1200.0f;
constexpr float kComicDefaultGapDip = 12.0f;
constexpr float kComicMinWidthFactor = 0.50f;
constexpr float kComicMaxWidthFactor = 2.00f;
constexpr std::size_t kComicSoftCacheBytes = 256ULL * 1024ULL * 1024ULL;
constexpr std::size_t kApplicationSoftCacheBytes = 512ULL * 1024ULL * 1024ULL;

struct ComicPageSource {
    std::wstring key;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    bool decode_failed = false;
};

struct ComicViewport {
    float width = 1.0f;
    float height = 1.0f;
    float dpi_scale = 1.0f;
};

struct ComicPageRange {
    int first = 0;
    int last = 0;

    bool empty() const noexcept { return first >= last; }
    int size() const noexcept { return std::max(0, last - first); }
    bool contains(int index) const noexcept {
        return index >= first && index < last;
    }
};

struct ComicPageGeometry {
    int index = -1;
    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    bool decode_failed = false;
};

struct ComicAnchor {
    std::wstring key;
    int index = -1;
    float page_fraction = 0.0f;
    float viewport_fraction = 0.5f;

    bool valid() const noexcept { return index >= 0; }
};

enum class ComicScrollDirection {
    Backward,
    Stationary,
    Forward,
};

class ComicReaderModel {
public:
    void set_pages(std::vector<ComicPageSource> pages) {
        const ComicAnchor anchor = capture_anchor();
        m_pages = std::move(pages);
        rebuild_layout();
        if (m_pages.empty()) {
            m_enabled = false;
            m_scroll = 0.0f;
            return;
        }
        if (m_enabled && anchor.valid()) restore_anchor(anchor);
    }

    void set_viewport(ComicViewport viewport) {
        const ComicAnchor anchor = capture_anchor();
        viewport.width = std::max(1.0f, viewport.width);
        viewport.height = std::max(1.0f, viewport.height);
        viewport.dpi_scale = std::max(0.01f, viewport.dpi_scale);
        m_viewport = viewport;
        rebuild_layout();
        if (m_enabled && anchor.valid()) restore_anchor(anchor);
    }

    bool enter(int current_index) {
        if (current_index < 0
            || current_index >= static_cast<int>(m_pages.size())) return false;
        m_enabled = true;
        m_scroll_direction = ComicScrollDirection::Stationary;
        m_scroll = clamp_scroll(m_page_tops[static_cast<std::size_t>(current_index)]);
        return true;
    }

    int exit_current_index() {
        const ComicAnchor anchor = capture_anchor();
        m_enabled = false;
        return anchor.valid() ? anchor.index : -1;
    }

    bool enabled() const noexcept { return m_enabled; }
    float scroll() const noexcept { return m_scroll; }
    float total_height() const noexcept { return m_total_height; }
    float width_factor() const noexcept { return m_width_factor; }
    float page_width() const noexcept { return target_page_width(); }
    float page_gap() const noexcept {
        return (m_seamless ? 0.0f : kComicDefaultGapDip) * m_viewport.dpi_scale;
    }
    bool seamless() const noexcept { return m_seamless; }
    ComicScrollDirection scroll_direction() const noexcept {
        return m_scroll_direction;
    }

    void set_scroll(float scroll) {
        const float next = clamp_scroll(scroll);
        if (next > m_scroll) {
            m_scroll_direction = ComicScrollDirection::Forward;
        } else if (next < m_scroll) {
            m_scroll_direction = ComicScrollDirection::Backward;
        }
        m_scroll = next;
    }

    void scroll_by(float delta) { set_scroll(m_scroll + delta); }
    void page_up() { scroll_by(-m_viewport.height); }
    void page_down() { scroll_by(m_viewport.height); }
    void home() { set_scroll(0.0f); }
    void end() { set_scroll(max_scroll()); }

    void set_width_factor(float factor) {
        const ComicAnchor anchor = capture_anchor();
        m_width_factor = std::clamp(
            factor, kComicMinWidthFactor, kComicMaxWidthFactor);
        rebuild_layout();
        if (m_enabled && anchor.valid()) restore_anchor(anchor);
    }

    void reset_width() { set_width_factor(1.0f); }

    void set_seamless(bool seamless) {
        if (m_seamless == seamless) return;
        const ComicAnchor anchor = capture_anchor();
        m_seamless = seamless;
        rebuild_layout();
        if (m_enabled && anchor.valid()) restore_anchor(anchor);
    }

    ComicAnchor capture_anchor(float viewport_fraction = 0.5f) const {
        ComicAnchor anchor;
        if (m_pages.empty() || m_page_tops.empty()) return anchor;
        anchor.viewport_fraction = std::clamp(viewport_fraction, 0.0f, 1.0f);
        const float position = m_scroll
            + m_viewport.height * anchor.viewport_fraction;
        const int index = page_at(position);
        if (index < 0) return anchor;
        const std::size_t offset = static_cast<std::size_t>(index);
        anchor.key = m_pages[offset].key;
        anchor.index = index;
        anchor.page_fraction = std::clamp(
            (position - m_page_tops[offset]) / m_page_heights[offset],
            0.0f, 1.0f);
        return anchor;
    }

    void restore_anchor(const ComicAnchor& anchor) {
        if (!anchor.valid() || m_pages.empty()) return;
        int index = -1;
        if (!anchor.key.empty()) {
            const auto found = std::find_if(
                m_pages.begin(), m_pages.end(), [&anchor](const ComicPageSource& page) {
                    return page.key == anchor.key;
                });
            if (found != m_pages.end()) {
                index = static_cast<int>(found - m_pages.begin());
            }
        }
        if (index < 0) {
            index = std::clamp(anchor.index, 0, static_cast<int>(m_pages.size()) - 1);
        }
        const std::size_t offset = static_cast<std::size_t>(index);
        const float page_position = m_page_tops[offset]
            + m_page_heights[offset] * std::clamp(anchor.page_fraction, 0.0f, 1.0f);
        m_scroll = clamp_scroll(page_position
            - m_viewport.height * std::clamp(anchor.viewport_fraction, 0.0f, 1.0f));
    }

    ComicPageRange visible_range() const {
        return range_for_interval(m_scroll, m_scroll + m_viewport.height);
    }

    ComicPageRange request_range() const {
        const bool backward = m_scroll_direction == ComicScrollDirection::Backward;
        const float before = (backward ? 2.0f : 1.0f) * m_viewport.height;
        const float after = (backward ? 1.0f : 2.0f) * m_viewport.height;
        return range_for_interval(
            std::max(0.0f, m_scroll - before),
            std::min(m_total_height, m_scroll + m_viewport.height + after));
    }

    std::optional<ComicPageGeometry> geometry(int index) const {
        if (index < 0 || index >= static_cast<int>(m_pages.size())) {
            return std::nullopt;
        }
        const std::size_t offset = static_cast<std::size_t>(index);
        const float width = target_page_width();
        return ComicPageGeometry{
            index,
            (m_viewport.width - width) * 0.5f,
            m_page_tops[offset],
            width,
            m_page_heights[offset],
            m_pages[offset].decode_failed};
    }

    std::vector<ComicPageGeometry> materialize(ComicPageRange range) const {
        range.first = std::clamp(range.first, 0, static_cast<int>(m_pages.size()));
        range.last = std::clamp(range.last, range.first, static_cast<int>(m_pages.size()));
        std::vector<ComicPageGeometry> result;
        result.reserve(static_cast<std::size_t>(range.size()));
        for (int index = range.first; index < range.last; ++index) {
            const auto page = geometry(index);
            if (page) result.push_back(*page);
        }
        return result;
    }

private:
    float target_page_width() const noexcept {
        const float base = std::min(
            m_viewport.width * kComicDefaultViewportRatio,
            kComicDefaultMaxWidthDip * m_viewport.dpi_scale);
        return std::max(1.0f, base * m_width_factor);
    }

    float clamp_scroll(float scroll) const noexcept {
        return std::clamp(scroll, 0.0f, max_scroll());
    }

    float max_scroll() const noexcept {
        return std::max(0.0f, m_total_height - m_viewport.height);
    }

    int page_at(float position) const {
        if (m_pages.empty()) return -1;
        const auto next = std::upper_bound(
            m_page_tops.begin(), m_page_tops.end(), position);
        int index = next == m_page_tops.begin()
            ? 0 : static_cast<int>(next - m_page_tops.begin()) - 1;
        index = std::clamp(index, 0, static_cast<int>(m_pages.size()) - 1);
        const std::size_t offset = static_cast<std::size_t>(index);
        const float bottom = m_page_tops[offset] + m_page_heights[offset];
        if (position > bottom && index + 1 < static_cast<int>(m_pages.size())) {
            const float next_top = m_page_tops[static_cast<std::size_t>(index + 1)];
            if (next_top - position < position - bottom) ++index;
        }
        return index;
    }

    ComicPageRange range_for_interval(float start, float end) const {
        if (m_pages.empty() || end <= start) return {};
        const auto first_it = std::upper_bound(m_page_bottoms.begin(), m_page_bottoms.end(), start);
        const auto last_it = std::lower_bound(m_page_tops.begin(), m_page_tops.end(), end);
        const int first = static_cast<int>(first_it - m_page_bottoms.begin());
        const int last = static_cast<int>(last_it - m_page_tops.begin());
        return ComicPageRange{
            std::clamp(first, 0, static_cast<int>(m_pages.size())),
            std::clamp(last, first, static_cast<int>(m_pages.size()))};
    }

    void rebuild_layout() {
        m_page_tops.clear();
        m_page_heights.clear();
        m_page_bottoms.clear();
        m_page_tops.reserve(m_pages.size());
        m_page_heights.reserve(m_pages.size());
        m_page_bottoms.reserve(m_pages.size());

        const float width = target_page_width();
        const float gap = page_gap();
        float top = 0.0f;
        for (std::size_t index = 0; index < m_pages.size(); ++index) {
            const ComicPageSource& page = m_pages[index];
            const float height = page.width > 0 && page.height > 0
                ? std::max(1.0f, width * static_cast<float>(page.height)
                    / static_cast<float>(page.width))
                : width;
            m_page_tops.push_back(top);
            m_page_heights.push_back(height);
            m_page_bottoms.push_back(top + height);
            top += height;
            if (index + 1 < m_pages.size()) top += gap;
        }
        m_total_height = top;
        m_scroll = clamp_scroll(m_scroll);
    }

    std::vector<ComicPageSource> m_pages;
    std::vector<float> m_page_tops;
    std::vector<float> m_page_heights;
    std::vector<float> m_page_bottoms;
    ComicViewport m_viewport;
    float m_scroll = 0.0f;
    float m_total_height = 0.0f;
    float m_width_factor = 1.0f;
    bool m_enabled = false;
    bool m_seamless = false;
    ComicScrollDirection m_scroll_direction = ComicScrollDirection::Stationary;
};

class ComicLruBudget {
public:
    ComicLruBudget(
        std::size_t comic_soft_limit = kComicSoftCacheBytes,
        std::size_t application_soft_limit = kApplicationSoftCacheBytes)
        : m_comic_soft_limit(comic_soft_limit),
          m_application_soft_limit(application_soft_limit) {}

    void touch(int index, std::size_t bytes) {
        Entry& entry = m_entries[index];
        m_resident_bytes -= entry.bytes;
        entry.bytes = bytes;
        entry.last_use = ++m_clock;
        m_resident_bytes += entry.bytes;
    }

    bool contains(int index) const { return m_entries.contains(index); }
    std::size_t resident_bytes() const noexcept { return m_resident_bytes; }

    std::size_t allowed_bytes(std::size_t other_private_bytes) const noexcept {
        const std::size_t remaining = other_private_bytes >= m_application_soft_limit
            ? 0 : m_application_soft_limit - other_private_bytes;
        return std::min(m_comic_soft_limit, remaining);
    }

    std::vector<int> evict_to_budget(
        std::size_t other_private_bytes, ComicPageRange protected_range) {
        const std::size_t limit = allowed_bytes(other_private_bytes);
        std::vector<std::pair<std::uint64_t, int>> candidates;
        candidates.reserve(m_entries.size());
        for (const auto& [index, entry] : m_entries) {
            if (!protected_range.contains(index)) {
                candidates.emplace_back(entry.last_use, index);
            }
        }
        std::sort(candidates.begin(), candidates.end());

        std::vector<int> evicted;
        for (const auto& [unused_tick, index] : candidates) {
            static_cast<void>(unused_tick);
            if (m_resident_bytes <= limit) break;
            const auto found = m_entries.find(index);
            if (found == m_entries.end()) continue;
            m_resident_bytes -= found->second.bytes;
            m_entries.erase(found);
            evicted.push_back(index);
        }
        return evicted;
    }

private:
    struct Entry {
        std::size_t bytes = 0;
        std::uint64_t last_use = 0;
    };

    std::unordered_map<int, Entry> m_entries;
    std::size_t m_resident_bytes = 0;
    std::size_t m_comic_soft_limit = kComicSoftCacheBytes;
    std::size_t m_application_soft_limit = kApplicationSoftCacheBytes;
    std::uint64_t m_clock = 0;
};

} // namespace mv
