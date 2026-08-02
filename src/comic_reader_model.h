#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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
constexpr float kComicCruiseBaseSpeedDipPerSecond = 120.0f;
constexpr float kComicMaxAutoScrollDeltaSeconds = 0.10f;
constexpr float kComicMiddleDeadZoneDip = 16.0f;
constexpr float kComicMiddleResponsePerSecond = 8.0f;
constexpr float kComicMiddleMaxSpeedDipPerSecond = 1200.0f;
constexpr float kComicMaxFiniteCoordinate =
    std::numeric_limits<float>::max() / 64.0f;
constexpr std::array<float, 4> kComicCruiseSpeedMultipliers{
    0.5f, 1.0f, 1.5f, 2.0f};
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

enum class ComicAutoScrollOwner {
    None,
    Cruise,
    Middle,
};

enum class ComicAutoScrollCancelReason {
    None,
    ToggleOff,
    ManualInput,
    Scrollbar,
    ReplacedByCruise,
    ReplacedByMiddle,
    RepeatedMiddleClick,
    LeftButton,
    Escape,
    KeyboardPage,
    MouseWheel,
    FocusLost,
    ViewportChanged,
    ExitMode,
    EmptyBook,
    Boundary,
    InvalidInput,
};

struct ComicPageStatus {
    int anchored_index = -1;
    int total_pages = 0;
};

struct ComicPageChangeEvent {
    int previous_index = -1;
    int current_index = -1;
    int total_pages = 0;
};

struct ComicScrollMetrics {
    float total_height = 0.0f;
    float viewport_height = 0.0f;
    float scroll = 0.0f;
    float max_scroll = 0.0f;
    bool is_valid = false;

    bool scrollable() const noexcept {
        return is_valid && max_scroll > 0.0f;
    }
};

inline bool comic_finite_coordinate(float value) noexcept {
    return std::isfinite(value)
        && std::fabs(value) <= kComicMaxFiniteCoordinate;
}

inline ComicScrollMetrics normalize_comic_scroll_metrics(
    float total_height, float viewport_height, float scroll) noexcept {
    ComicScrollMetrics metrics;
    if (!comic_finite_coordinate(total_height)
        || !comic_finite_coordinate(viewport_height)
        || !comic_finite_coordinate(scroll)
        || total_height < 0.0f || viewport_height < 0.0f) {
        return metrics;
    }
    const double maximum = std::max(
        0.0, static_cast<double>(total_height)
            - static_cast<double>(viewport_height));
    metrics.total_height = total_height;
    metrics.viewport_height = viewport_height;
    metrics.max_scroll = static_cast<float>(maximum);
    metrics.scroll = static_cast<float>(std::clamp(
        static_cast<double>(scroll), 0.0, maximum));
    metrics.is_valid = true;
    return metrics;
}

class ComicReaderModel {
public:
    void set_pages(std::vector<ComicPageSource> pages) {
        const int previous_page = current_page_index();
        const ComicAnchor anchor = capture_anchor();
        m_pages = std::move(pages);
        rebuild_layout();
        if (m_pages.empty()) {
            cancel_auto_scroll(ComicAutoScrollCancelReason::EmptyBook);
            m_enabled = false;
            m_scroll = 0.0f;
            record_page_change(previous_page);
            return;
        }
        if (m_enabled && anchor.valid()) restore_anchor_impl(anchor);
        record_page_change(previous_page);
    }

    void set_viewport(ComicViewport viewport) {
        viewport.width = sanitize_positive(viewport.width, m_viewport.width);
        viewport.height = sanitize_positive(viewport.height, m_viewport.height);
        viewport.dpi_scale = sanitize_positive(
            viewport.dpi_scale, m_viewport.dpi_scale);
        if (viewport.width == m_viewport.width
            && viewport.height == m_viewport.height
            && viewport.dpi_scale == m_viewport.dpi_scale) return;
        const int previous_page = current_page_index();
        const ComicAnchor anchor = capture_anchor();
        m_viewport = viewport;
        rebuild_layout();
        if (m_enabled && anchor.valid()) restore_anchor_impl(anchor);
        record_page_change(previous_page);
    }

    bool enter(int current_index) {
        if (current_index < 0
            || current_index >= static_cast<int>(m_pages.size())) return false;
        const int previous_page = current_page_index();
        m_enabled = true;
        m_scroll_direction = ComicScrollDirection::Stationary;
        m_scroll = clamp_scroll(m_page_tops[static_cast<std::size_t>(current_index)]);
        record_page_change(previous_page);
        return true;
    }

    int exit_current_index() {
        const ComicAnchor anchor = capture_anchor();
        cancel_auto_scroll(ComicAutoScrollCancelReason::ExitMode);
        m_enabled = false;
        return anchor.valid() ? anchor.index : -1;
    }

    bool enabled() const noexcept { return m_enabled; }
    float scroll() const noexcept { return m_scroll; }
    float total_height() const noexcept { return m_total_height; }
    float width_factor() const noexcept { return m_width_factor; }
    float page_width() const noexcept { return target_page_width(); }
    float page_gap() const noexcept {
        return safe_product(
            m_seamless ? 0.0f : kComicDefaultGapDip,
            m_viewport.dpi_scale, 0.0f);
    }
    bool seamless() const noexcept { return m_seamless; }
    ComicScrollDirection scroll_direction() const noexcept {
        return m_scroll_direction;
    }

    ComicPageStatus page_status() const {
        return ComicPageStatus{
            current_page_index(), static_cast<int>(m_pages.size())};
    }

    int current_page_index() const {
        if (m_pages.empty() || m_page_tops.empty()) return -1;
        return page_at(anchor_position(0.5f));
    }
    int total_pages() const noexcept { return static_cast<int>(m_pages.size()); }

    std::optional<ComicPageChangeEvent> take_page_change_event() noexcept {
        std::optional<ComicPageChangeEvent> event = m_page_change_event;
        m_page_change_event.reset();
        return event;
    }

    void set_scroll(float scroll) {
        cancel_auto_scroll(ComicAutoScrollCancelReason::ManualInput);
        apply_scroll(scroll);
    }

    void scroll_by(float delta) {
        cancel_auto_scroll(ComicAutoScrollCancelReason::ManualInput);
        apply_scroll(safe_sum(m_scroll, delta, m_scroll));
    }
    void scroll_to_page(int index) {
        cancel_auto_scroll(ComicAutoScrollCancelReason::ManualInput);
        if (index < 0 || index >= static_cast<int>(m_pages.size())) return;
        apply_scroll(m_page_tops[static_cast<std::size_t>(index)]);
    }
    void page_up() { scroll_by(-m_viewport.height); }
    void page_down() { scroll_by(m_viewport.height); }
    void home() { set_scroll(0.0f); }
    void end() { set_scroll(max_scroll()); }

    ComicScrollMetrics scroll_metrics() const noexcept {
        return normalize_comic_scroll_metrics(
            m_total_height, m_viewport.height, m_scroll);
    }

    void set_scroll_from_scrollbar(float mapped_scroll) {
        cancel_auto_scroll(ComicAutoScrollCancelReason::Scrollbar);
        apply_scroll(mapped_scroll);
    }

    void scrollbar_page_step(ComicScrollDirection direction) {
        cancel_auto_scroll(ComicAutoScrollCancelReason::Scrollbar);
        if (direction == ComicScrollDirection::Backward) {
            apply_scroll(safe_sum(m_scroll, -m_viewport.height, m_scroll));
        } else if (direction == ComicScrollDirection::Forward) {
            apply_scroll(safe_sum(m_scroll, m_viewport.height, m_scroll));
        }
    }

    int cruise_speed_index() const noexcept { return m_cruise_speed_index; }
    float cruise_speed_multiplier() const noexcept {
        return kComicCruiseSpeedMultipliers[
            static_cast<std::size_t>(m_cruise_speed_index)];
    }

    void set_cruise_speed_index(int index) noexcept {
        m_cruise_speed_index = std::clamp(
            index, 0, static_cast<int>(kComicCruiseSpeedMultipliers.size()) - 1);
    }

    void change_cruise_speed(int delta) noexcept {
        const long long requested = static_cast<long long>(m_cruise_speed_index)
            + static_cast<long long>(delta);
        const long long maximum = static_cast<long long>(
            kComicCruiseSpeedMultipliers.size() - 1);
        m_cruise_speed_index = static_cast<int>(
            std::clamp(requested, 0LL, maximum));
    }

    ComicAutoScrollOwner auto_scroll_owner() const noexcept {
        return m_auto_scroll_owner;
    }
    ComicAutoScrollCancelReason last_auto_scroll_cancel_reason() const noexcept {
        return m_last_auto_scroll_cancel_reason;
    }
    bool cruise_active() const noexcept {
        return m_auto_scroll_owner == ComicAutoScrollOwner::Cruise;
    }
    bool middle_autoscroll_active() const noexcept {
        return m_auto_scroll_owner == ComicAutoScrollOwner::Middle;
    }

    bool start_cruise() noexcept {
        const bool replaced_middle =
            m_auto_scroll_owner == ComicAutoScrollOwner::Middle;
        if (replaced_middle) {
            cancel_auto_scroll(ComicAutoScrollCancelReason::ReplacedByCruise);
        }
        if (!m_enabled || max_scroll() <= m_scroll) return false;
        if (!replaced_middle
            && m_auto_scroll_owner != ComicAutoScrollOwner::Cruise) {
            m_last_auto_scroll_cancel_reason = ComicAutoScrollCancelReason::None;
        }
        m_auto_scroll_owner = ComicAutoScrollOwner::Cruise;
        return true;
    }

    bool toggle_cruise() noexcept {
        if (cruise_active()) {
            cancel_auto_scroll(ComicAutoScrollCancelReason::ToggleOff);
            return false;
        }
        return start_cruise();
    }

    bool start_middle_autoscroll(float anchor_y) noexcept {
        if (!m_enabled || max_scroll() <= 0.0f
            || !comic_finite_coordinate(anchor_y)) return false;
        if (m_auto_scroll_owner == ComicAutoScrollOwner::Middle) {
            cancel_auto_scroll(
                ComicAutoScrollCancelReason::RepeatedMiddleClick);
            return false;
        }
        if (m_auto_scroll_owner == ComicAutoScrollOwner::Cruise) {
            cancel_auto_scroll(ComicAutoScrollCancelReason::ReplacedByMiddle);
        } else {
            m_last_auto_scroll_cancel_reason = ComicAutoScrollCancelReason::None;
        }
        m_middle_anchor_y = anchor_y;
        m_auto_scroll_owner = ComicAutoScrollOwner::Middle;
        return true;
    }

    void cancel_auto_scroll(ComicAutoScrollCancelReason reason) noexcept {
        if (m_auto_scroll_owner == ComicAutoScrollOwner::None) return;
        m_auto_scroll_owner = ComicAutoScrollOwner::None;
        m_middle_anchor_y = 0.0f;
        m_last_auto_scroll_cancel_reason = reason;
    }

    static float capped_elapsed_seconds(float elapsed_seconds) noexcept {
        if (!std::isfinite(elapsed_seconds) || elapsed_seconds <= 0.0f) {
            return 0.0f;
        }
        return std::min(elapsed_seconds, kComicMaxAutoScrollDeltaSeconds);
    }

    // Returns the signed physical-pixel delta applied after dt and boundary caps.
    float advance_cruise(float elapsed_seconds) {
        if (!cruise_active()) return 0.0f;
        const float elapsed = capped_elapsed_seconds(elapsed_seconds);
        if (elapsed <= 0.0f) return 0.0f;
        const double velocity = static_cast<double>(
            kComicCruiseBaseSpeedDipPerSecond)
            * static_cast<double>(cruise_speed_multiplier())
            * static_cast<double>(m_viewport.dpi_scale);
        return advance_auto_scroll(velocity, elapsed);
    }

    static float middle_autoscroll_velocity_from_offset(
        float pointer_offset, float dpi_scale) noexcept {
        if (!comic_finite_coordinate(pointer_offset)
            || !comic_finite_coordinate(dpi_scale) || dpi_scale <= 0.0f) {
            return 0.0f;
        }
        const double scale = dpi_scale;
        const double offset_dip = static_cast<double>(pointer_offset) / scale;
        const double distance_dip = std::fabs(offset_dip);
        const double excess_dip = std::max(
            0.0, distance_dip - static_cast<double>(kComicMiddleDeadZoneDip));
        const double speed_dip = std::min(
            static_cast<double>(kComicMiddleMaxSpeedDipPerSecond),
            excess_dip * static_cast<double>(kComicMiddleResponsePerSecond));
        const double velocity = std::copysign(speed_dip * scale, offset_dip);
        if (!std::isfinite(velocity)) return 0.0f;
        return static_cast<float>(std::clamp(
            velocity, -static_cast<double>(kComicMaxFiniteCoordinate),
            static_cast<double>(kComicMaxFiniteCoordinate)));
    }

    float middle_autoscroll_velocity(float pointer_y) const noexcept {
        if (!middle_autoscroll_active()
            || !comic_finite_coordinate(pointer_y)) return 0.0f;
        const double offset = static_cast<double>(pointer_y)
            - static_cast<double>(m_middle_anchor_y);
        if (!std::isfinite(offset)
            || std::fabs(offset) > kComicMaxFiniteCoordinate) return 0.0f;
        return middle_autoscroll_velocity_from_offset(
            static_cast<float>(offset), m_viewport.dpi_scale);
    }

    // Returns the signed physical-pixel delta applied after dt and boundary caps.
    float advance_middle_autoscroll(
        float pointer_y, float elapsed_seconds) {
        if (!middle_autoscroll_active()) return 0.0f;
        if (!comic_finite_coordinate(pointer_y)
            || !std::isfinite(elapsed_seconds)) {
            cancel_auto_scroll(ComicAutoScrollCancelReason::InvalidInput);
            return 0.0f;
        }
        const float elapsed = capped_elapsed_seconds(elapsed_seconds);
        if (elapsed <= 0.0f) return 0.0f;
        const float velocity = middle_autoscroll_velocity(pointer_y);
        if (velocity == 0.0f) return 0.0f;
        return advance_auto_scroll(velocity, elapsed);
    }

    void set_width_factor(float factor) {
        if (!std::isfinite(factor)) return;
        const int previous_page = current_page_index();
        const ComicAnchor anchor = capture_anchor();
        m_width_factor = std::clamp(
            factor, kComicMinWidthFactor, kComicMaxWidthFactor);
        rebuild_layout();
        if (m_enabled && anchor.valid()) restore_anchor_impl(anchor);
        record_page_change(previous_page);
    }

    void reset_width() { set_width_factor(1.0f); }

    void set_seamless(bool seamless) {
        if (m_seamless == seamless) return;
        const int previous_page = current_page_index();
        const ComicAnchor anchor = capture_anchor();
        m_seamless = seamless;
        rebuild_layout();
        if (m_enabled && anchor.valid()) restore_anchor_impl(anchor);
        record_page_change(previous_page);
    }

    void update_page(
        int index, std::uint32_t width, std::uint32_t height,
        bool decode_failed) {
        if (index < 0 || index >= static_cast<int>(m_pages.size())) return;
        const int previous_page = current_page_index();
        const ComicAnchor anchor = capture_anchor();
        ComicPageSource& page = m_pages[static_cast<std::size_t>(index)];
        page.width = width;
        page.height = height;
        page.decode_failed = decode_failed;
        rebuild_layout();
        if (m_enabled && anchor.valid()) restore_anchor_impl(anchor);
        record_page_change(previous_page);
    }

    ComicAnchor capture_anchor(float viewport_fraction = 0.5f) const {
        ComicAnchor anchor;
        if (m_pages.empty() || m_page_tops.empty()) return anchor;
        anchor.viewport_fraction = std::isfinite(viewport_fraction)
            ? std::clamp(viewport_fraction, 0.0f, 1.0f) : 0.5f;
        const float position = anchor_position(anchor.viewport_fraction);
        const int index = page_at(position);
        if (index < 0) return anchor;
        const std::size_t offset = static_cast<std::size_t>(index);
        anchor.key = m_pages[offset].key;
        anchor.index = index;
        const float page_fraction =
            (position - m_page_tops[offset]) / m_page_heights[offset];
        anchor.page_fraction = std::isfinite(page_fraction)
            ? std::clamp(page_fraction, 0.0f, 1.0f) : 0.0f;
        return anchor;
    }

    void restore_anchor(const ComicAnchor& anchor) {
        const int previous_page = current_page_index();
        restore_anchor_impl(anchor);
        record_page_change(previous_page);
    }

    ComicPageRange visible_range() const {
        return range_for_interval(
            m_scroll, safe_sum(m_scroll, m_viewport.height, m_total_height));
    }

    ComicPageRange request_range() const {
        const bool backward = m_scroll_direction == ComicScrollDirection::Backward;
        const float before = safe_product(
            backward ? 2.0f : 1.0f, m_viewport.height, 0.0f);
        const float after = safe_product(
            backward ? 1.0f : 2.0f, m_viewport.height, 0.0f);
        return range_for_interval(
            std::max(0.0f, safe_sum(m_scroll, -before, 0.0f)),
            std::min(m_total_height, safe_sum(
                safe_sum(m_scroll, m_viewport.height, m_total_height),
                after, m_total_height)));
    }

    std::optional<ComicPageGeometry> geometry(int index) const {
        if (index < 0 || index >= static_cast<int>(m_pages.size())) {
            return std::nullopt;
        }
        const std::size_t offset = static_cast<std::size_t>(index);
        const float width = target_page_width();
        return ComicPageGeometry{
            index,
            safe_product(safe_sum(m_viewport.width, -width, 0.0f), 0.5f, 0.0f),
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
    static float sanitize_positive(float value, float fallback) noexcept {
        if (!comic_finite_coordinate(value) || value <= 0.0f) return fallback;
        return value;
    }

    static float safe_product(
        float left, float right, float fallback) noexcept {
        if (!comic_finite_coordinate(left) || !comic_finite_coordinate(right)) {
            return fallback;
        }
        const double product = static_cast<double>(left)
            * static_cast<double>(right);
        if (!std::isfinite(product)) return fallback;
        return static_cast<float>(std::clamp(
            product, -static_cast<double>(kComicMaxFiniteCoordinate),
            static_cast<double>(kComicMaxFiniteCoordinate)));
    }

    static float safe_sum(
        float left, float right, float fallback) noexcept {
        if (!comic_finite_coordinate(left) || !comic_finite_coordinate(right)) {
            return fallback;
        }
        const double sum = static_cast<double>(left) + static_cast<double>(right);
        if (!std::isfinite(sum)) return fallback;
        return static_cast<float>(std::clamp(
            sum, -static_cast<double>(kComicMaxFiniteCoordinate),
            static_cast<double>(kComicMaxFiniteCoordinate)));
    }

    float anchor_position(float viewport_fraction) const noexcept {
        const float fraction = std::isfinite(viewport_fraction)
            ? std::clamp(viewport_fraction, 0.0f, 1.0f) : 0.5f;
        return safe_sum(
            m_scroll,
            safe_product(m_viewport.height, fraction, 0.0f),
            m_scroll);
    }

    void restore_anchor_impl(const ComicAnchor& anchor) {
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
        const float page_fraction = std::isfinite(anchor.page_fraction)
            ? std::clamp(anchor.page_fraction, 0.0f, 1.0f) : 0.0f;
        const float viewport_fraction = std::isfinite(anchor.viewport_fraction)
            ? std::clamp(anchor.viewport_fraction, 0.0f, 1.0f) : 0.5f;
        const float page_position = safe_sum(
            m_page_tops[offset],
            safe_product(m_page_heights[offset], page_fraction, 0.0f),
            m_page_tops[offset]);
        m_scroll = clamp_scroll(safe_sum(
            page_position,
            -safe_product(m_viewport.height, viewport_fraction, 0.0f),
            m_scroll));
    }

    float target_page_width() const noexcept {
        const float base = std::min(
            safe_product(
                m_viewport.width, kComicDefaultViewportRatio, 1.0f),
            safe_product(
                kComicDefaultMaxWidthDip, m_viewport.dpi_scale, 1.0f));
        return std::max(1.0f, safe_product(base, m_width_factor, 1.0f));
    }

    float clamp_scroll(float scroll) const noexcept {
        if (!comic_finite_coordinate(scroll)) return m_scroll;
        return std::clamp(scroll, 0.0f, max_scroll());
    }

    float max_scroll() const noexcept {
        return std::max(
            0.0f, safe_sum(m_total_height, -m_viewport.height, 0.0f));
    }

    void apply_scroll(float scroll) {
        const int previous_page = current_page_index();
        const float next = clamp_scroll(scroll);
        if (next > m_scroll) {
            m_scroll_direction = ComicScrollDirection::Forward;
        } else if (next < m_scroll) {
            m_scroll_direction = ComicScrollDirection::Backward;
        }
        m_scroll = next;
        record_page_change(previous_page);
    }

    float advance_auto_scroll(double velocity, float elapsed_seconds) {
        if (!std::isfinite(velocity)) {
            cancel_auto_scroll(ComicAutoScrollCancelReason::InvalidInput);
            return 0.0f;
        }
        const float before = m_scroll;
        const double requested = static_cast<double>(before)
            + velocity * static_cast<double>(elapsed_seconds);
        if (!std::isfinite(requested)) {
            cancel_auto_scroll(ComicAutoScrollCancelReason::InvalidInput);
            return 0.0f;
        }
        apply_scroll(static_cast<float>(std::clamp(
            requested, 0.0, static_cast<double>(max_scroll()))));
        const float applied = m_scroll - before;
        if ((velocity > 0.0 && m_scroll >= max_scroll())
            || (velocity < 0.0 && m_scroll <= 0.0f)) {
            cancel_auto_scroll(ComicAutoScrollCancelReason::Boundary);
        }
        return applied;
    }

    void record_page_change(int previous_page) {
        const int current_page = current_page_index();
        if (current_page == previous_page) {
            if (m_page_change_event) {
                m_page_change_event->total_pages =
                    static_cast<int>(m_pages.size());
            }
            return;
        }
        m_page_change_event = ComicPageChangeEvent{
            previous_page, current_page, static_cast<int>(m_pages.size())};
    }

    int page_at(float position) const {
        if (m_pages.empty() || !comic_finite_coordinate(position)) return -1;
        const auto next = std::upper_bound(
            m_page_tops.begin(), m_page_tops.end(), position);
        int index = next == m_page_tops.begin()
            ? 0 : static_cast<int>(next - m_page_tops.begin()) - 1;
        index = std::clamp(index, 0, static_cast<int>(m_pages.size()) - 1);
        const std::size_t offset = static_cast<std::size_t>(index);
        const float bottom = safe_sum(
            m_page_tops[offset], m_page_heights[offset], m_page_bottoms[offset]);
        if (position > bottom && index + 1 < static_cast<int>(m_pages.size())) {
            const float next_top = m_page_tops[static_cast<std::size_t>(index + 1)];
            if (next_top - position < position - bottom) ++index;
        }
        return index;
    }

    ComicPageRange range_for_interval(float start, float end) const {
        if (m_pages.empty() || !comic_finite_coordinate(start)
            || !comic_finite_coordinate(end) || end <= start) return {};
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
                ? std::max(1.0f, safe_product(
                    width,
                    static_cast<float>(page.height)
                        / static_cast<float>(page.width),
                    width))
                : width;
            m_page_tops.push_back(top);
            m_page_heights.push_back(height);
            const float bottom = safe_sum(top, height, kComicMaxFiniteCoordinate);
            m_page_bottoms.push_back(bottom);
            top = bottom;
            if (index + 1 < m_pages.size()) {
                top = safe_sum(top, gap, kComicMaxFiniteCoordinate);
            }
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
    ComicAutoScrollOwner m_auto_scroll_owner = ComicAutoScrollOwner::None;
    ComicAutoScrollCancelReason m_last_auto_scroll_cancel_reason =
        ComicAutoScrollCancelReason::None;
    std::optional<ComicPageChangeEvent> m_page_change_event;
    int m_cruise_speed_index = 1;
    float m_middle_anchor_y = 0.0f;
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

    void erase(int index) {
        const auto found = m_entries.find(index);
        if (found == m_entries.end()) return;
        m_resident_bytes -= found->second.bytes;
        m_entries.erase(found);
    }

    void clear() {
        m_entries.clear();
        m_resident_bytes = 0;
        m_clock = 0;
    }

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
