// Unit tests for the filmstrip geometry model (Issue #5 P1).
// Pure logic — no Windows dependencies. Run via CTest (filmstrip_model.unit).

#include "filmstrip_model.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void expect_near(float actual, float expected, float tolerance, const char* message) {
    if (!std::isfinite(actual) || !std::isfinite(expected)
        || std::fabs(actual - expected) > tolerance) {
        throw std::runtime_error(message);
    }
}

void test_empty_model() {
    mv::FilmstripModel model;
    expect(model.empty(), "empty model reports empty");
    expect(model.item_count() == 0, "empty model item count");
    expect(model.total_width() == 0.0f, "empty model total width");
    expect(model.max_scroll() == 0.0f, "empty model max scroll");
    expect(model.left_overflow() == false, "empty model no left overflow");
    expect(model.right_overflow() == false, "empty model no right overflow");
    const auto [first, last] = model.visible_range();
    expect(first == 0 && last == 0, "empty model visible range empty");
    expect(model.hit_test(10.0f) == -1, "empty model hit test miss");
    expect(model.current() == -1, "empty model no current");
}

void test_basic_layout() {
    mv::FilmstripModel model;
    model.set_items(10);
    model.set_viewport(1000.0f, 1.0f);  // 96 DPI
    expect(model.total_width() == 10.0f * 64.0f + 9.0f * 8.0f,
        "10 square items at 96dpi: 10*56 + 9*8");
    expect_near(model.thumb_height(), 64.0f, 0.001f, "thumb height 56 dip at 96dpi");
    // All items fit: no overflow, scroll stays 0
    expect(model.left_overflow() == false, "no left overflow when fit");
    expect(model.right_overflow() == false, "no right overflow when fit");
    expect(model.max_scroll() == 0.0f, "no scroll range when fit");

    const mv::FilmstripItemRect r0 = model.item_rect(0);
    expect_near(r0.left, 0.0f, 0.001f, "first item at x=0");
    expect_near(r0.width, 64.0f, 0.001f, "square item width 56");
    expect_near(r0.top, 8.0f, 0.001f, "item top at 8 dip padding");
    expect_near(r0.height, 64.0f, 0.001f, "item height 56");
    expect(r0.current == false, "item 0 not current");

    const mv::FilmstripItemRect r3 = model.item_rect(3);
    expect_near(r3.left, 3.0f * (64.0f + 8.0f), 0.001f, "item 3 left offset");
}

void test_aspect_ratio() {
    mv::FilmstripModel model;
    model.set_items(3);
    model.set_viewport(1000.0f, 1.0f);
    model.set_item_aspect(1, 2.0f);   // wide
    model.set_item_aspect(2, 0.5f);   // tall
    const mv::FilmstripItemRect r0 = model.item_rect(0);
    const mv::FilmstripItemRect r1 = model.item_rect(1);
    const mv::FilmstripItemRect r2 = model.item_rect(2);
    expect_near(r0.width, 64.0f, 0.001f, "default aspect width 56");
    expect_near(r1.width, 128.0f, 0.001f, "wide item width 2*56");
    expect_near(r2.width, 32.0f, 0.001f, "tall item width 0.5*56");
    expect_near(r2.left, 64.0f + 8.0f + 128.0f + 8.0f, 0.001f, "tall item x after wide");
    // Aspect clamp: absurd values are bounded, layout stays finite
    model.set_item_aspect(0, 1000.0f);
    expect(std::isfinite(model.total_width()), "clamped aspect keeps width finite");
    expect_near(model.item_rect(0).width, 64.0f * mv::kFilmstripMaxAspect, 0.001f,
        "aspect clamped to max");
    // Invalid updates are ignored
    model.set_item_aspect(-1, 3.0f);
    model.set_item_aspect(99, 3.0f);
    model.set_item_aspect(0, 0.0f);
    model.set_item_aspect(0, -1.0f);
    expect_near(model.item_rect(0).width, 64.0f * mv::kFilmstripMaxAspect, 0.001f,
        "invalid aspect updates ignored");
}

void test_windowed_visible_range() {
    mv::FilmstripModel model;
    model.set_items(200);
    model.set_viewport(600.0f, 1.0f);
    const auto [first0, last0] = model.visible_range();
    expect(first0 == 0, "visible range starts at 0 when at home");
    // ~600 / 64 per slot ≈ 9-10 items + margin
    expect(last0 > 5 && last0 < 15, "windowed visible range only renders ~10 items");

    model.set_scroll(200.0f * (64.0f + 8.0f) * 0.5f);  // scroll to middle
    const auto [first1, last1] = model.visible_range();
    expect(first1 > 0, "scrolled viewport starts past the beginning");
    expect(last1 - first1 < 20, "visible window stays small while scrolling");
    expect(last1 <= 200, "visible range never exceeds item count");

    model.end();
    const auto [first2, last2] = model.visible_range();
    expect(last2 == 200, "at end the visible range reaches the last item");
    expect(first2 > 180, "at end the visible range is near the tail");
}

void test_center_current() {
    mv::FilmstripModel model;
    model.set_items(100);
    model.set_viewport(640.0f, 1.0f);
    model.set_current(50);
    model.advance_animation(1.0f);  // finish scroll/zoom transition
    const mv::FilmstripItemRect r = model.item_rect(50);
    const float center = r.left + r.width * 0.5f;
    expect_near(center, 640.0f * 0.5f, 1.0f,
        "current item center sits at viewport center");
    // Centered at the ends (no clamping): the first item stays at the
    // viewport center even though that leaves empty space to its left.
    model.set_current(0);
    model.advance_animation(1.0f);  // finish scroll/zoom transition
    const mv::FilmstripItemRect r0 = model.item_rect(0);
    expect_near(r0.left + r0.width * 0.5f, 640.0f * 0.5f, 1.0f,
        "first item stays centered (no clamp)");
    expect(model.left_overflow() == false, "at first item left overflow hidden");
    expect(model.right_overflow(), "at first item right overflow shown");
    model.set_current(99);
    for (int i = 0; i < 600 && model.advance_animation(1.0f / 60.0f); ++i) {
    }  // finish scroll/zoom transition
    const mv::FilmstripItemRect r99 = model.item_rect(99);
    expect_near(r99.left + r99.width * 0.5f, 640.0f * 0.5f, 1.0f,
        "last item stays centered (no clamp)");
    expect(model.left_overflow(), "at last item left overflow shown");
    expect(model.right_overflow() == false, "at last item right overflow hidden");
    // Home: left overflow hidden, right shown
    model.home();
    expect(model.scroll() == 0.0f, "home scrolls to 0");
    expect(model.left_overflow() == false, "at home left overflow hidden");
    expect(model.right_overflow(), "at home right overflow shown");
}

void test_overflow_logic() {
    mv::FilmstripModel model;
    model.set_items(50);
    model.set_viewport(400.0f, 1.0f);  // 50 items (3200px) >> viewport
    expect(model.left_overflow() == false, "overflow: left hidden at 0");
    expect(model.right_overflow(), "overflow: right shown at 0");
    model.set_scroll(model.max_scroll() * 0.5f);
    expect(model.left_overflow(), "overflow: both shown mid-scroll");
    expect(model.right_overflow(), "overflow: both shown mid-scroll");
    model.set_scroll(model.max_scroll());
    expect(model.left_overflow(), "overflow: left shown at end");
    expect(model.right_overflow() == false, "overflow: right hidden at end");
    // Just barely scrolled: epsilon guard keeps flags stable
    model.set_scroll(0.2f);
    expect(model.left_overflow() == false, "overflow epsilon: tiny scroll no arrow");
}

void test_panel_avoidance() {
    mv::FilmstripModel model;
    model.set_items(15);  // 15*56 + 14*8 = 952 px total
    model.set_viewport(1200.0f, 1.0f);
    const auto [first_wide, last_wide] = model.visible_range();
    expect(last_wide - first_wide == 15, "wide viewport shows all items");
    expect(model.right_overflow() == false, "wide viewport no right arrow");
    // Side panel expands: strip width shrinks
    model.set_viewport(600.0f, 1.0f);
    const auto [first_narrow, last_narrow] = model.visible_range();
    expect(last_narrow - first_narrow < 15, "narrow viewport windows the items");
    expect(model.right_overflow(), "narrow viewport shows right arrow");
    // After a shrink, the current item is re-centered (main view sync rule)
    model.set_current(5);
    model.advance_animation(1.0f);  // finish scroll/zoom transition
    const mv::FilmstripItemRect r = model.item_rect(5);
    expect_near(r.left + r.width * 0.5f, 600.0f * 0.5f, 1.0f,
        "viewport shrink re-centers current item");
}

void test_hit_test() {
    mv::FilmstripModel model;
    model.set_items(20);
    model.set_viewport(1000.0f, 1.0f);
    model.set_current(7);
    // Hit a plain item
    expect(model.hit_test(5.0f) == 0, "hit test item 0");
    // Hit the magnified current item (its slot is 56 wide, magnified to 70)
    const mv::FilmstripItemRect cur = model.item_rect(7);
    expect(model.hit_test(cur.left + 1.0f) == 7, "hit test current item (left edge)");
    expect(model.hit_test(cur.left + cur.width - 1.0f) == 7,
        "hit test current item (right edge)");
    // Hit the magnified overhang: inside magnified rect but outside base slot
    expect(model.hit_test(cur.left + 1.0f) == 7, "hit test overhang left");
    // Gap / whitespace misses
    const mv::FilmstripItemRect n0 = model.item_rect(0);
    expect(model.hit_test(n0.left + n0.width + 4.0f) == -1,
        "gap between items misses");
    // Beyond content
    expect(model.hit_test(-100.0f) == -1, "negative x misses");
    expect(model.hit_test(5000.0f) == -1, "far x misses");
}

void test_scroll_by_step() {
    mv::FilmstripModel model;
    model.set_items(100);
    model.set_viewport(640.0f, 1.0f);
    model.set_current(10);
    model.scroll_by(1.0f);
    expect(model.current() == 11, "scroll_by(+1) steps the current item forward");
    expect_near(model.scroll(), model.scroll_for_current(11), 0.001f,
        "stepped item stays centered");
    model.scroll_by(-1.0f);
    expect(model.current() == 10, "scroll_by(-1) steps back");
    model.scroll_by(1000.0f);
    expect(model.current() == 99, "large positive delta clamps at the last item");
    model.scroll_by(-1000.0f);
    expect(model.current() == 0, "large negative delta clamps at the first item");
}

void test_dpi_scaling() {
    mv::FilmstripModel model;
    model.set_items(10);
    model.set_viewport(2000.0f, 2.0f);  // 200% DPI
    expect_near(model.thumb_height(), 128.0f, 0.001f, "thumb height doubles at 200%");
    expect_near(model.total_width(), 10.0f * 128.0f + 9.0f * 16.0f, 0.001f,
        "total width doubles at 200%");
    const mv::FilmstripItemRect r = model.item_rect(0);
    expect_near(r.top, 16.0f, 0.001f, "padding doubles at 200%");
    expect_near(r.height, 128.0f, 0.001f, "item height doubles at 200%");
    model.set_current(5);
    model.advance_animation(1.0f);  // finish zoom transition
    const mv::FilmstripItemRect cur = model.item_rect(5);
    expect_near(cur.width, 128.0f * mv::kFilmstripCurrentScale, 0.001f,
        "current item width scaled at 200%");
}

void test_magnification_geometry() {
    mv::FilmstripModel model;
    model.set_items(5);
    model.set_viewport(2000.0f, 1.0f);
    model.set_current(2);
    model.advance_animation(1.0f);  // finish zoom transition
    const mv::FilmstripItemRect cur = model.item_rect(2);
    const mv::FilmstripItemRect other = model.item_rect(1);
    expect_near(cur.width, other.width * mv::kFilmstripCurrentScale, 0.001f,
        "current item width is 1.25x");
    expect_near(cur.height, other.height * mv::kFilmstripCurrentScale, 0.001f,
        "current item height is 1.25x");
    // Magnification stays centered on its slot relative to the strip; in
    // strip-local coordinates the current item sits at the viewport center.
    const float mag_center = cur.left + cur.width * 0.5f;
    expect_near(mag_center, 2000.0f * 0.5f, 0.001f,
        "magnified current item centers in viewport");
}

void test_gaps_constant_during_transition() {
    // The per-side push keeps EVERY gap at its settled value for the whole
    // transition, INCLUDING the switch instant (t=0). A widening variant
    // would produce 16/24dip gaps at t=0 and fail these asserts. The test
    // replays a REAL switch (index differs), so the previous-item shrink
    // branch is actually exercised (same-index set_current would not).
    mv::FilmstripModel model;
    model.set_items(6);
    model.set_viewport(2000.0f, 1.0f);
    model.set_current(2);
    model.advance_animation(1.0f);  // settle on 2
    model.set_current(3, true);     // transition 2 -> 3
    model.advance_animation(1.0f);  // finish
    const mv::FilmstripItemRect c0 = model.item_rect(3);
    const mv::FilmstripItemRect n0 = model.item_rect(4);
    const float settled = n0.left - (c0.left + c0.width);
    expect_near(settled, 8.0f, 0.5f, "settled current-right gap is 8dip");
    // Real replay: transition 3 -> 4 (prev = 3). Sample t=0 first (the
    // switch instant that used to snap to 16/24dip), then every 10%.
    model.set_current(4, true);
    auto gap_cur_right = [&]() {
        const mv::FilmstripItemRect cur = model.item_rect(4);
        const mv::FilmstripItemRect nb = model.item_rect(5);
        return nb.left - (cur.left + cur.width);
    };
    auto gap_prev_cur = [&]() {
        const mv::FilmstripItemRect cur = model.item_rect(4);
        const mv::FilmstripItemRect prev = model.item_rect(3);
        return cur.left - (prev.left + prev.width);
    };
    auto gap_prev_left = [&]() {
        const mv::FilmstripItemRect prev = model.item_rect(3);
        const mv::FilmstripItemRect left = model.item_rect(2);
        return prev.left - (left.left + left.width);
    };
    expect_near(gap_cur_right(), settled, 0.5f,
        "t=0 current-right gap constant at switch instant");
    expect_near(gap_prev_cur(), settled, 0.5f,
        "t=0 prev-current gap constant at switch instant");
    expect_near(gap_prev_left(), settled, 0.5f,
        "t=0 prev-left gap constant at switch instant");
    for (int step = 1; step <= 10; ++step) {
        model.advance_animation(0.03f);  // 300ms -> 30ms per 10%
        expect_near(gap_cur_right(), settled, 0.5f,
            "current-right gap constant through transition");
        expect_near(gap_prev_cur(), settled, 0.5f,
            "prev-current gap constant through transition");
        expect_near(gap_prev_left(), settled, 0.5f,
            "prev-left gap constant through transition");
    }
}

void test_viewport_change_keeps_current_centered() {
    mv::FilmstripModel model;
    model.set_items(100);
    model.set_viewport(800.0f, 1.0f);
    model.set_current(30);
    const float before = model.scroll();
    model.set_viewport(500.0f, 1.0f);
    model.advance_animation(1.0f);  // finish scroll/zoom transition
    const mv::FilmstripItemRect r = model.item_rect(30);
    expect_near(r.left + r.width * 0.5f, 500.0f * 0.5f, 1.0f,
        "current item re-centered after viewport change");
    expect(model.scroll() != before, "scroll adjusted after viewport change");
}

} // namespace

int main() {
    int failures = 0;
    const struct {
        const char* name;
        void (*fn)();
    } cases[] = {
        {"empty_model", test_empty_model},
        {"basic_layout", test_basic_layout},
        {"aspect_ratio", test_aspect_ratio},
        {"windowed_visible_range", test_windowed_visible_range},
        {"center_current", test_center_current},
        {"overflow_logic", test_overflow_logic},
        {"panel_avoidance", test_panel_avoidance},
        {"hit_test", test_hit_test},
        {"scroll_by_step", test_scroll_by_step},
        {"dpi_scaling", test_dpi_scaling},
        {"magnification_geometry", test_magnification_geometry},
        {"gaps_constant_during_transition", test_gaps_constant_during_transition},
        {"viewport_change_keeps_current_centered",
            test_viewport_change_keeps_current_centered},
    };
    for (const auto& test : cases) {
        try {
            test.fn();
            std::cout << "[PASS] " << test.name << "\n";
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << "\n";
            ++failures;
        }
    }
    if (failures == 0) {
        std::cout << "filmstrip_model: all " << sizeof(cases) / sizeof(cases[0])
                  << " tests passed\n";
        return 0;
    }
    std::cerr << "filmstrip_model: " << failures << " test(s) failed\n";
    return 1;
}
