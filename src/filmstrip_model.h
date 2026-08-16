#pragma once

// Filmstrip model — pure geometry/scroll logic for the large-image bottom
// filmstrip (Issue #5 P1). Header-only and dependency-free so it can be
// unit-tested standalone (mirrors the comic_reader_model seam).
//
// All geometry is computed in physical pixels: inputs (viewport width,
// dpi scale) are physical, DIP constants are scaled by dpi/96 internally.
// The model owns:
//   - item layout (aspect-preserving thumbnails, fixed max height)
//   - windowed visible range (only visible items are rendered)
//   - scroll position + centering of the current item
//   - overflow flags (edge gradient + arrows)
//   - hit testing (with the 1.25x current-item magnification)

#include "design_tokens.h"
#include "layout.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace mv {

// Filmstrip geometry constants live in layout.h (single source of truth);
// this header re-exports the ones the model needs for convenience.
using layout::kFilmstripCurrentScale;
using layout::kFilmstripGapDip;
using layout::kFilmstripBorderDip;
using layout::kFilmstripHeightDip;
using layout::kFilmstripMaxAspect;
using layout::kFilmstripPadVDip;
using layout::kFilmstripThumbHeightDip;

struct FilmstripItemRect {
    int index = -1;
    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    bool current = false;
    float zoom = 1.0f;   // current/prev magnified zoom (for border alpha)
};

class FilmstripModel {
public:
    void set_items(int count) {
        const int bounded = std::max(0, count);
        if (bounded == m_count) return;
        m_count = bounded;
        m_aspects.assign(static_cast<std::size_t>(m_count), 1.0f);
        m_current = -1;
        m_prev_current = -1;
        m_scroll = 0.0f;
        m_display_scroll = 0.0f;
        m_scroll_from = 0.0f;
        m_scroll_target = 0.0f;
        m_anim_time = 1.0f;
        m_zoom_t = 1.0f;
        rebuild();
    }

    void set_item_aspect(int index, float aspect) {
        if (index < 0 || index >= m_count) return;
        if (!std::isfinite(aspect) || aspect <= 0.0f) return;
        const float clamped = std::min(aspect, kFilmstripMaxAspect);
        if (clamped == m_aspects[static_cast<std::size_t>(index)]) return;
        m_aspects[static_cast<std::size_t>(index)] = clamped;
        rebuild();
        // The current item's slot width changed, so re-center it (its
        // center must stay locked to the viewport center, which is the
        // big-image region's center).
        if (index == m_current) {
            m_scroll = scroll_for_current(index);
        }
    }

    void set_viewport(float width, float dpi_scale) {
        const float bounded_width = std::max(0.0f, width);
        const float bounded_dpi = (std::isfinite(dpi_scale) && dpi_scale > 0.0f)
            ? dpi_scale : 1.0f;
        if (bounded_width == m_viewport && bounded_dpi == m_dpi_scale) return;
        m_viewport = bounded_width;
        m_dpi_scale = bounded_dpi;
        rebuild();
        if (m_current >= 0) set_current(m_current);
        else set_scroll(m_scroll);
    }

    // Scroll position that centers the given item (clamped to scroll bounds).
    // Returns the scroll that centers the given item, WITHOUT clamping:
    // the first/last items stay centered too, leaving empty space on the
    // outer side (user requirement). Programmatic jumps (home/end/set_scroll)
    // still clamp via max_scroll().
    float scroll_for_current(int index) const {
        if (index < 0 || index >= m_count) return m_scroll;
        const float base_w = m_thumb_h * aspect(index);
        const float center = m_x[static_cast<std::size_t>(index)] + base_w * 0.5f;
        return center - m_viewport * 0.5f;
    }

    // Centers the given item in the viewport (no clamping at the ends).
    // With animate=true the strip plays the full handoff transition: the
    // strip scrolls forward one slot, the previous item's border fades
    // out while it shrinks 1.25x -> 1.0x, and the new item's border fades
    // in while it grows 1.0x -> 1.25x (all driven by advance_animation).
    // With animate=false (wheel paging) the switch is instant.
    void set_current(int index, bool animate = true) {
        if (index < 0 || index >= m_count) return;
        if (m_current != index) {
            if (animate) {
                // The old item becomes the shrinking previous item. Its
                // starting zoom is the CURRENT on-screen zoom (1.25 after a
                // completed switch, or an in-flight value under rapid
                // paging) — never a hard jump back to 1.25.
                m_prev_zoom_start = current_zoom();
                m_prev_current = m_current;  // old item shrinks back to 1.0
                m_zoom_t = 0.0f;             // new item grows 1.0 -> scale
            } else {
                m_prev_current = -1;
                m_prev_zoom_start = 1.0f;
                m_zoom_t = 1.0f;  // instant, no zoom animation while paging
            }
            m_current = index;
        }
        // Always re-center (also when the index is unchanged but the
        // viewport/aspect changed). The animation interpolates from the
        // current on-screen position to the new centered target.
        const float target = scroll_for_current(index);
        m_scroll_from = m_display_scroll;
        m_scroll_target = target;
        m_scroll = target;
        m_anim_time = 0.0f;
    }

    // Wheel browsing steps the current item (NAVIGATION_DESIGN 2.1.3:
    // "按缩略图步进"), keeping it centered in the viewport. A large delta
    // (fast wheel) advances multiple items at once.
    void scroll_by(float delta) {
        if (m_count == 0 || !std::isfinite(delta)) return;
        int steps = static_cast<int>(delta);
        if (steps == 0) steps = (delta > 0) ? 1 : -1;
        const int target = std::clamp(m_current + steps, 0, m_count - 1);
        if (target != m_current) set_current(target, /*animate=*/false);
    }

    void set_scroll(float scroll) {
        if (!std::isfinite(scroll)) return;
        m_scroll = std::clamp(scroll, 0.0f, max_scroll());
        // Programmatic jumps (home/end/restore) are immediate; only
        // user navigation via set_current animates.
        m_display_scroll = m_scroll;
        m_scroll_from = m_scroll;
        m_scroll_target = m_scroll;
        m_anim_time = 1.0f;  // no animation pending
    }

    // Advance the scroll/zoom transition. Returns true while animating so
    // the caller can keep invalidating. Time-driven (not per-frame step):
    // the whole handoff — strip scroll of one slot, old item shrink +
    // border fade-out, new item grow + border fade-in — completes in a
    // fixed 300ms with the same ease-out quartic interpolator as the
    // big-image transitions, regardless of render frame rate.
    bool advance_animation(float dt) {
        bool animating = false;
        constexpr float kDur = mv::dt::kDurationFilmstripHandoffSec;
        if (m_anim_time < 1.0f) {
            animating = true;
            m_anim_time = std::min(1.0f, m_anim_time + dt / kDur);
            const float et = 1.0f - (1.0f - m_anim_time) * (1.0f - m_anim_time)
                * (1.0f - m_anim_time) * (1.0f - m_anim_time);
            m_display_scroll = m_scroll_from
                + (m_scroll_target - m_scroll_from) * et;
            m_zoom_t = m_anim_time;
            if (m_anim_time >= 1.0f) {
                m_display_scroll = m_scroll_target;
                m_zoom_t = 1.0f;
                m_prev_current = -1;  // shrink finished
            }
        }
        return animating;
    }

    // The rendered scroll position (animates toward scroll()).
    float display_scroll() const noexcept { return m_display_scroll; }

    // Zoom factor for the current item: 1.0 -> kFilmstripCurrentScale, eased.
    float current_zoom() const noexcept {
        const float t = m_zoom_t;
        const float et = 1.0f - (1.0f - t) * (1.0f - t)
            * (1.0f - t) * (1.0f - t);
        return 1.0f + (kFilmstripCurrentScale - 1.0f) * et;
    }
    // Zoom factor for the previously selected item (shrinks back to 1.0).
    // Starts from the zoom it had when it became "previous" (the old
    // current), so rapid paging never snaps it back to 1.25.
    float prev_zoom() const noexcept {
        const float t = m_zoom_t;
        const float et = 1.0f - (1.0f - t) * (1.0f - t)
            * (1.0f - t) * (1.0f - t);
        return 1.0f + (m_prev_zoom_start - 1.0f) * (1.0f - et);
    }

    void home() { set_scroll(0.0f); }
    void end() { set_scroll(max_scroll()); }

    bool empty() const noexcept { return m_count == 0; }
    int item_count() const noexcept { return m_count; }
    int current() const noexcept { return m_current; }
    // Previous selection index (valid only while the switch animation runs).
    int prev_index() const noexcept { return m_prev_current; }
    // Animation progress 0..1 for the current selection switch.
    float anim_t() const noexcept { return m_zoom_t; }
    // True while the scroll/zoom transition is still running.
    bool animating() const noexcept { return m_anim_time < 1.0f; }
    float scroll() const noexcept { return m_scroll; }
    float thumb_height() const noexcept { return m_thumb_h; }
    float gap() const noexcept { return kFilmstripGapDip * m_dpi_scale; }
    float dpi_scale() const noexcept { return m_dpi_scale; }

    float total_width() const noexcept { return m_total_w; }

    float max_scroll() const noexcept {
        return std::max(0.0f, m_total_w - m_viewport);
    }

    bool left_overflow() const noexcept { return m_scroll > 0.5f; }
    bool right_overflow() const noexcept {
        return m_total_w - m_viewport - m_scroll > 0.5f;
    }

    // Layout rect for one item in strip-local coordinates (includes the
    // 1.25x magnification of the current item, centered on its base slot).
    FilmstripItemRect item_rect(int index) const {
        FilmstripItemRect rect;
        if (index < 0 || index >= m_count) return rect;
        const float pad_v = kFilmstripPadVDip * m_dpi_scale;
        const float content_h =
            kFilmstripHeightDip * m_dpi_scale - 2.0f * pad_v;
        const float base_w = m_thumb_h * aspect(index);
        rect.index = index;
        rect.current = (index == m_current);
        // All rects are strip-local: subtract the displayed scroll offset so
        // the viewport follows the (animated) scroll and set_current centers
        // the current item.
        const float local_scroll = m_display_scroll;
        float zoom = 1.0f;
        if (index == m_current) {
            zoom = current_zoom();
        } else if (index == m_prev_current && m_zoom_t < 1.0f) {
            zoom = prev_zoom();
        }
        if (index == m_current) {
            // Current item: symmetric magnification centered on its base
            // slot (floating over neighbors). Its left/right extension is
            // compensated by the push propagation below. While the switch
            // animation runs, the previous item's extension also pushes the
            // current slot so the instant of selection change does not snap
            // the target item's gaps (settled 8dip -> 16/24dip).
            float push_prev = 0.0f;
            if (m_prev_current >= 0 && m_zoom_t < 1.0f) {
                const float ext_prev = m_thumb_h * aspect(m_prev_current)
                    * (prev_zoom() - 1.0f) * 0.5f;
                if (index > m_prev_current) push_prev = ext_prev;
                else if (index < m_prev_current) push_prev = -ext_prev;
            }
            const float center =
                m_x[static_cast<std::size_t>(index)] + base_w * 0.5f + push_prev;
            const float w = base_w * zoom;
            const float h = m_thumb_h * zoom;
            rect.left = center - w * 0.5f - local_scroll;
            rect.top = pad_v + (content_h - h);
            rect.width = w;
            rect.height = h;
            rect.zoom = zoom;
        } else {
            rect.left = m_x[static_cast<std::size_t>(index)] - local_scroll;
            // Exact gap-preserving push: every gap stays at the normal
            // spacing for the WHOLE transition. Current-side neighbors
            // follow the current item's animated growth, prev sits at the
            // constant push, items beyond prev absorb the prev shrink.
            const int cur = m_current;
            if (cur >= 0) {
                const bool prev_active =
                    m_prev_current >= 0 && m_zoom_t < 1.0f;
                const float ext_cur = m_thumb_h * aspect(cur)
                    * (current_zoom() - 1.0f) * 0.5f;
                const float ext_prev = prev_active
                    ? m_thumb_h * aspect(m_prev_current)
                        * (prev_zoom() - 1.0f) * 0.5f
                    : 0.0f;
                if (index > cur) {
                    if (prev_active && index == m_prev_current)
                        rect.left += ext_cur + ext_prev;
                    else if (prev_active && index > m_prev_current)
                        rect.left += ext_cur + ext_prev;
                    else
                        rect.left += ext_cur;
                } else if (index < cur) {
                    if (prev_active && index == m_prev_current)
                        rect.left -= ext_cur + ext_prev;
                    else if (prev_active && index < m_prev_current)
                        rect.left -= ext_cur + ext_prev;
                    else
                        rect.left -= ext_cur;
                }
            }
            // The shrinking previous item keeps its magnification for
            // DRAWING (floats over the fixed gap); its layout above is a
            // normal fixed slot. The drawn size animates with prev_zoom
            // (80 -> 64) so the gap toward the target never snaps wider at
            // the switch instant.
            rect.top = pad_v + (content_h - m_thumb_h * zoom);
            rect.width = base_w * zoom;
            rect.height = m_thumb_h * zoom;
            rect.zoom = zoom;
        }
        return rect;
    }

    // Windowed visible range [first, last) — only these items need to be
    // rendered or have thumbnails requested. The margin covers the current
    // item's magnification overhang. Layout coordinates are absolute
    // (0-based); the viewport spans [scroll, scroll + viewport].
    std::pair<int, int> visible_range() const {
        if (m_count == 0) return {0, 0};
        const float margin = m_thumb_h * (kFilmstripCurrentScale - 1.0f) * 0.5f
            + kFilmstripBorderDip * 0.5f
            + kFilmstripGapDip * m_dpi_scale;
        const float max_w = m_thumb_h * kFilmstripMaxAspect;
        // Render window follows the displayed (animated) scroll so items
        // slide in/out during the transition instead of popping.
        const float window_left = m_display_scroll;
        const auto it_first = std::lower_bound(
            m_x.begin(), m_x.end(), window_left - margin - max_w);
        int first = static_cast<int>(it_first - m_x.begin());
        // item_rect() returns strip-LOCAL coordinates (already shifted by
        // -m_scroll); the strip viewport spans [0, viewport] locally, with
        // the magnification margin extending it. Do not compare local rects
        // against absolute window bounds (that only works when scroll == 0).
        while (first < m_count) {
            const FilmstripItemRect r = item_rect(first);
            if (r.left + r.width >= -margin) break;
            ++first;
        }
        int last = first;
        while (last < m_count) {
            const FilmstripItemRect r = item_rect(last);
            if (r.left > m_viewport + margin) break;
            ++last;
        }
        if (last < first) last = first;
        return {first, last};
    }

    // Hit test in strip-local coordinates; returns item index or -1.
    int hit_test(float x) const {
        const auto [first, last] = visible_range();
        for (int i = first; i < last; ++i) {
            const FilmstripItemRect r = item_rect(i);
            if (x >= r.left && x < r.left + r.width) return i;
        }
        return -1;
    }

private:
    float aspect(int index) const noexcept {
        return m_aspects[static_cast<std::size_t>(index)];
    }

    void rebuild() {
        const float gap = kFilmstripGapDip * m_dpi_scale;
        m_thumb_h = kFilmstripThumbHeightDip * m_dpi_scale;
        m_x.assign(static_cast<std::size_t>(m_count), 0.0f);
        float cursor = 0.0f;
        for (int i = 0; i < m_count; ++i) {
            m_x[static_cast<std::size_t>(i)] = cursor;
            cursor += m_thumb_h * aspect(i) + gap;
        }
        m_total_w = m_count > 0 ? cursor - gap : 0.0f;
    }

    int m_count = 0;
    float m_viewport = 0.0f;
    float m_dpi_scale = 1.0f;
    float m_thumb_h = kFilmstripThumbHeightDip;
    float m_total_w = 0.0f;
    float m_scroll = 0.0f;
    float m_display_scroll = 0.0f;   // animated rendering scroll
    float m_scroll_from = 0.0f;      // animation start (on-screen) scroll
    float m_scroll_target = 0.0f;    // animation target (centered) scroll
    float m_anim_time = 1.0f;        // animation progress 0..1 (1 = idle)
    float m_zoom_t = 1.0f;           // current-item zoom anim 0..1 (1 = done)
    int m_prev_current = -1;         // previous selection (shrinks during anim)
    float m_prev_zoom_start = 1.0f;  // prev zoom when it became "previous"
    int m_current = -1;
    std::vector<float> m_aspects;
    std::vector<float> m_x;  // left edge of each item (unscaled slot)
};

} // namespace mv
