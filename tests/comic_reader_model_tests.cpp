#include "comic_reader_model.h"

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
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

std::vector<mv::ComicPageSource> make_pages(int count) {
    std::vector<mv::ComicPageSource> pages;
    pages.reserve(static_cast<std::size_t>(count));
    for (int index = 0; index < count; ++index) {
        pages.push_back({L"page-" + std::to_wstring(index), 1000, 1000, false});
    }
    return pages;
}

std::string read_source(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("failed to open production source");
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

void test_width_gap_and_failure_geometry() {
    mv::ComicReaderModel model;
    model.set_viewport({1600.0f, 900.0f, 1.0f});
    model.set_pages({
        {L"tall", 1000, 2000, true},
        {L"unknown", 0, 0, true}});

    expect_near(model.page_width(), 1200.0f, 0.01f,
        "default width must clamp to 1200 DIP");
    expect_near(model.page_gap(), 12.0f, 0.01f,
        "default gap must be 12 DIP");
    const auto failed = model.geometry(0);
    expect(failed.has_value() && failed->decode_failed,
        "a failed page must retain a renderable geometry entry");
    expect_near(failed->height, 2400.0f, 0.01f,
        "a failure card must retain the probed page height");
    const auto unknown = model.geometry(1);
    expect(unknown.has_value(), "an unprobeable failed page must still have geometry");
    expect_near(unknown->height, model.page_width(), 0.01f,
        "an unprobeable failed page must use a stable square fallback");

    model.set_width_factor(0.01f);
    expect_near(model.width_factor(), 0.5f, 0.001f,
        "reader width must clamp to 50 percent");
    model.set_width_factor(9.0f);
    expect_near(model.width_factor(), 2.0f, 0.001f,
        "reader width must clamp to 200 percent");
    model.reset_width();
    expect_near(model.width_factor(), 1.0f, 0.001f,
        "reset must restore the default width factor");
    model.set_seamless(true);
    expect_near(model.page_gap(), 0.0f, 0.001f,
        "seamless mode must remove the page gap");

    model.set_viewport({3000.0f, 1800.0f, 2.0f});
    expect_near(model.page_width(), 2400.0f, 0.01f,
        "the 1200 DIP width cap must scale with DPI");
}

void test_anchor_survives_width_viewport_and_reordering() {
    mv::ComicReaderModel model;
    model.set_viewport({1000.0f, 800.0f, 1.0f});
    model.set_pages({
        {L"a", 1000, 2000, false},
        {L"b", 1000, 2000, false},
        {L"c", 1000, 2000, false}});
    expect(model.enter(1), "enter must accept the current page index");
    model.scroll_by(500.0f);
    const mv::ComicAnchor before = model.capture_anchor();
    expect(before.key == L"b", "scrolling within a tall page must retain its key anchor");

    model.set_width_factor(1.5f);
    mv::ComicAnchor after_width = model.capture_anchor();
    expect(after_width.key == before.key,
        "width changes must preserve the visible page anchor");
    expect_near(after_width.page_fraction, before.page_fraction, 0.002f,
        "width changes must preserve the page-relative anchor");

    model.set_viewport({760.0f, 900.0f, 1.0f});
    const mv::ComicAnchor after_panel = model.capture_anchor();
    expect(after_panel.key == before.key,
        "panel, fullscreen, and resize viewport changes must preserve the page key");
    expect_near(after_panel.page_fraction, before.page_fraction, 0.002f,
        "viewport changes must preserve the page-relative anchor");

    model.set_pages({
        {L"c", 1000, 2000, false},
        {L"a", 1000, 2000, false},
        {L"b", 1000, 2000, false}});
    expect(model.capture_anchor().key == L"b",
        "sort rebuilds must preserve the anchor by stable page key");
    expect(model.exit_current_index() == 2,
        "exit must return the currently visible page after reordering");
}

void test_removed_anchor_selects_successor_and_empty_disables() {
    mv::ComicReaderModel model;
    model.set_viewport({1000.0f, 600.0f, 1.0f});
    model.set_pages({
        {L"a", 1000, 1000, false},
        {L"b", 1000, 1000, false},
        {L"c", 1000, 1000, false}});
    expect(model.enter(1), "reader must enter on page b");
    model.set_pages({
        {L"a", 1000, 1000, false},
        {L"c", 1000, 1000, false}});
    expect(model.capture_anchor().key == L"c",
        "deleting the anchor page must select the successor at the same index");
    model.set_pages({});
    expect(!model.enabled() && model.exit_current_index() == -1,
        "an empty recursive or delete result must disable the reader cleanly");
}

void test_scroll_commands_and_directional_request_window() {
    mv::ComicReaderModel model;
    model.set_viewport({1000.0f, 500.0f, 1.0f});
    model.set_pages(make_pages(20));
    expect(model.enter(5), "reader must enter at page five");

    const auto visible = model.visible_range();
    expect(visible.first == 5 && visible.last == 6,
        "visible range must contain only intersecting pages");
    const auto stationary = model.request_range();
    expect(stationary.first == 4 && stationary.last == 7,
        "stationary entry must request one viewport behind and two ahead");

    model.page_up();
    expect(model.scroll_direction() == mv::ComicScrollDirection::Backward,
        "PageUp must set the backward scheduling direction");
    const auto backward = model.request_range();
    expect(backward.first == 3 && backward.last == 6,
        "backward scrolling must request two viewports backward and one forward");

    model.page_down();
    model.page_down();
    expect(model.scroll_direction() == mv::ComicScrollDirection::Forward,
        "PageDown must set the forward scheduling direction");
    const auto forward = model.request_range();
    expect(forward.first == 5 && forward.last == 8,
        "forward scrolling must request one viewport backward and two forward");

    model.home();
    expect_near(model.scroll(), 0.0f, 0.001f, "Home must reach the first page");
    model.end();
    expect_near(model.scroll(), model.total_height() - 500.0f, 0.01f,
        "End must reach the final viewport");

    model.scroll_to_page(10);
    expect(model.capture_anchor(0.0f).index == 10,
        "direct navigation must place the requested page at the viewport start");
}

void test_decode_update_preserves_anchor() {
    mv::ComicReaderModel model;
    model.set_viewport({1000.0f, 600.0f, 1.0f});
    model.set_pages(make_pages(6));
    expect(model.enter(3), "reader must enter before a decoded size update");
    model.scroll_by(250.0f);
    const mv::ComicAnchor before = model.capture_anchor();

    model.update_page(1, 1000, 3000, false);
    const mv::ComicAnchor after_other_page = model.capture_anchor();
    expect(after_other_page.key == before.key,
        "a decoded size update before the viewport must preserve the page anchor");
    expect_near(after_other_page.page_fraction, before.page_fraction, 0.002f,
        "a decoded size update must preserve the page-relative anchor");

    model.update_page(before.index, 1000, 2500, true);
    const mv::ComicAnchor after_anchor_page = model.capture_anchor();
    expect(after_anchor_page.key == before.key,
        "updating the anchored page must preserve its stable key");
    expect_near(after_anchor_page.page_fraction, before.page_fraction, 0.002f,
        "updating the anchored page must preserve its relative position");
    const auto geometry = model.geometry(before.index);
    expect(geometry.has_value() && geometry->decode_failed,
        "a failed decode update must reach the production geometry state");
}

void test_page_status_and_change_event() {
    mv::ComicReaderModel model;
    model.set_viewport({1000.0f, 400.0f, 1.0f});
    model.set_pages(make_pages(3));
    const auto initial = model.take_page_change_event();
    expect(initial.has_value() && initial->previous_index == -1
            && initial->current_index == 0 && initial->total_pages == 3,
        "adding the first book must publish its initial anchored page");
    expect(model.enter(0), "page event model must enter its first page");
    expect(!model.take_page_change_event().has_value(),
        "entering the already anchored page must not duplicate an event");

    model.scroll_by(100.0f);
    expect(!model.take_page_change_event().has_value(),
        "scrolling within one anchored page must not publish an event");
    model.scroll_to_page(1);
    model.scroll_to_page(2);
    model.set_pages(make_pages(4));
    const auto changed = model.take_page_change_event();
    expect(changed.has_value() && changed->previous_index == 1
            && changed->current_index == 2 && changed->total_pages == 4,
        "unconsumed page changes must retain only the latest transition and total");
    expect(!model.take_page_change_event().has_value(),
        "consuming a page event must clear it instead of queueing duplicates");

    model.set_pages(make_pages(5));
    const mv::ComicPageStatus status = model.page_status();
    expect(status.anchored_index == 2 && status.total_pages == 5,
        "page status must report the restored zero-based anchor and total pages");
    expect(!model.take_page_change_event().has_value(),
        "a total-page change without an index change must not publish an event");
}

void test_cruise_speed_elapsed_cap_and_boundaries() {
    mv::ComicReaderModel model;
    model.set_viewport({1000.0f, 400.0f, 1.75f});
    model.set_pages(make_pages(5));
    expect(model.enter(0), "cruise model must enter a scrollable book");
    model.take_page_change_event();

    expect(model.cruise_speed_index() == 1
            && model.cruise_speed_multiplier() == 1.0f,
        "cruise must default to the 1.0x tier");
    struct CruiseTierCase {
        int index;
        float multiplier;
    };
    constexpr CruiseTierCase tiers[] = {
        {0, 0.5f},
        {1, 1.0f},
        {2, 1.5f},
        {3, 2.0f},
    };
    for (const CruiseTierCase& tier : tiers) {
        model.set_scroll_from_scrollbar(0.0f);
        model.set_cruise_speed_index(tier.index);
        expect_near(model.cruise_speed_multiplier(), tier.multiplier, 0.0f,
            "each cruise tier must expose its exact multiplier");
        expect(model.start_cruise(),
            "each cruise tier must start on a scrollable book");
        const float before = model.scroll();
        const float expected_delta = mv::kComicCruiseBaseSpeedDipPerSecond
            * tier.multiplier * 1.75f * 0.05f;
        const float applied = model.advance_cruise(0.05f);
        expect_near(applied, expected_delta, 0.001f,
            "each cruise tier must apply its exact elapsed-time distance");
        expect_near(model.scroll() - before, applied, 0.001f,
            "cruise advance must return the signed applied pixel delta");
        model.cancel_auto_scroll(mv::ComicAutoScrollCancelReason::ToggleOff);
    }
    model.set_cruise_speed_index(-20);
    expect(model.cruise_speed_index() == 0
            && model.cruise_speed_multiplier() == 0.5f,
        "cruise speed must clamp below the four tiers");
    model.change_cruise_speed(1);
    expect(model.cruise_speed_index() == 1,
        "speed increment must advance exactly one tier");
    model.set_cruise_speed_index(99);
    model.change_cruise_speed(99);
    expect(model.cruise_speed_index() == 3
            && model.cruise_speed_multiplier() == 2.0f,
        "cruise speed must clamp above the four tiers");
    model.set_cruise_speed_index(1);

    expect(model.start_cruise(), "cruise must start on a scrollable book");
    expect_near(model.advance_cruise(0.05f), 10.5f, 0.001f,
        "elapsed seconds must advance at 120 DIP/s times the DPI scale");
    expect_near(model.advance_cruise(5.0f), 21.0f, 0.001f,
        "a stalled frame must advance by no more than the 100 ms dt cap");
    const float before_invalid_elapsed = model.scroll();
    expect_near(model.advance_cruise(
        std::numeric_limits<float>::quiet_NaN()), 0.0f, 0.0f,
        "NaN elapsed time must fail closed");
    expect_near(model.scroll(), before_invalid_elapsed, 0.0f,
        "invalid elapsed time must not move the canvas");

    model.scroll_by(1.0f);
    expect(model.auto_scroll_owner() == mv::ComicAutoScrollOwner::None
            && model.last_auto_scroll_cancel_reason()
                == mv::ComicAutoScrollCancelReason::ManualInput,
        "manual scrolling must pause cruise ownership");

    const mv::ComicScrollMetrics metrics = model.scroll_metrics();
    model.set_scroll_from_scrollbar(metrics.max_scroll - 1.0f);
    expect(model.start_cruise(), "cruise must restart immediately before the end");
    expect_near(model.advance_cruise(0.1f), 1.0f, 0.001f,
        "cruise must clamp its final advance to the canvas end");
    expect(model.auto_scroll_owner() == mv::ComicAutoScrollOwner::None
            && model.last_auto_scroll_cancel_reason()
                == mv::ComicAutoScrollCancelReason::Boundary,
        "reaching the final canvas boundary must stop cruise");

    model.set_scroll_from_scrollbar(0.0f);
    expect(model.start_cruise(),
        "cruise must restart before the current book becomes empty");
    model.set_pages({});
    expect(!model.enabled()
            && model.auto_scroll_owner() == mv::ComicAutoScrollOwner::None
            && model.last_auto_scroll_cancel_reason()
                == mv::ComicAutoScrollCancelReason::EmptyBook,
        "an empty current book must disable the reader and stop cruise");
}

void test_autoscroll_ownership_and_middle_curve() {
    constexpr float dpi_scales[] = {1.0f, 1.75f, 2.0f};
    for (const float scale : dpi_scales) {
        const float dead_zone = mv::kComicMiddleDeadZoneDip * scale;
        expect_near(mv::ComicReaderModel::middle_autoscroll_velocity_from_offset(
            dead_zone, scale), 0.0f, 0.0f,
            "the 96/168/192 DPI dead zone must suppress pointer jitter");
        const float downward =
            mv::ComicReaderModel::middle_autoscroll_velocity_from_offset(
                dead_zone + 10.0f * scale, scale);
        const float farther =
            mv::ComicReaderModel::middle_autoscroll_velocity_from_offset(
                dead_zone + 20.0f * scale, scale);
        const float upward =
            mv::ComicReaderModel::middle_autoscroll_velocity_from_offset(
                -dead_zone - 10.0f * scale, scale);
        expect(downward > 0.0f && farther > downward && upward < 0.0f,
            "middle autoscroll must preserve direction and increase continuously");
        expect_near(
            mv::ComicReaderModel::middle_autoscroll_velocity_from_offset(
                mv::kComicMaxFiniteCoordinate, scale),
            mv::kComicMiddleMaxSpeedDipPerSecond * scale, 0.1f,
            "middle autoscroll speed must cap in physical pixels per second");
    }
    expect_near(mv::ComicReaderModel::middle_autoscroll_velocity_from_offset(
        std::numeric_limits<float>::infinity(), 1.0f), 0.0f, 0.0f,
        "non-finite middle offsets must fail closed");

    mv::ComicReaderModel model;
    model.set_viewport({1000.0f, 500.0f, 2.0f});
    model.set_pages(make_pages(5));
    expect(model.enter(0), "middle autoscroll model must enter a book");
    expect(model.start_cruise(), "cruise must own scrolling before replacement");
    expect(model.start_middle_autoscroll(100.0f)
            && model.auto_scroll_owner() == mv::ComicAutoScrollOwner::Middle
            && model.last_auto_scroll_cancel_reason()
                == mv::ComicAutoScrollCancelReason::ReplacedByMiddle,
        "starting middle autoscroll must replace cruise ownership");
    expect(!model.start_middle_autoscroll(100.0f)
            && model.last_auto_scroll_cancel_reason()
                == mv::ComicAutoScrollCancelReason::RepeatedMiddleClick,
        "a repeated middle click must cancel middle autoscroll");
    expect(model.start_middle_autoscroll(100.0f),
        "middle autoscroll must restart after cancellation");
    expect(model.start_cruise()
            && model.last_auto_scroll_cancel_reason()
                == mv::ComicAutoScrollCancelReason::ReplacedByCruise,
        "starting cruise must replace middle ownership");

    expect(model.start_middle_autoscroll(100.0f),
        "middle autoscroll must start for elapsed-time advancement");
    expect(model.advance_middle_autoscroll(152.0f, 0.05f) > 0.0f,
        "middle autoscroll must advance from pointer distance and elapsed time");
    model.set_scroll_from_scrollbar(1000.0f);
    expect(model.start_middle_autoscroll(100.0f),
        "middle autoscroll must start from an interior canvas position");
    const float before_upward = model.scroll();
    const float upward_applied = model.advance_middle_autoscroll(48.0f, 0.05f);
    expect(upward_applied < 0.0f,
        "an upward pointer offset from the interior must return a negative delta");
    expect_near(upward_applied, -8.0f, 0.001f,
        "middle upward distance must include the 2x DPI scale and elapsed time");
    expect_near(model.scroll() - before_upward, upward_applied, 0.001f,
        "middle advance must return the signed applied physical-pixel delta");
    model.set_scroll_from_scrollbar(0.0f);
    expect(model.start_middle_autoscroll(100.0f),
        "middle autoscroll must start at the top boundary");
    expect_near(model.advance_middle_autoscroll(0.0f, 0.05f), 0.0f, 0.0f,
        "upward middle autoscroll must clamp at the top boundary");
    expect(model.last_auto_scroll_cancel_reason()
            == mv::ComicAutoScrollCancelReason::Boundary,
        "reaching the directional boundary must cancel middle autoscroll");

    expect(model.start_middle_autoscroll(100.0f),
        "middle autoscroll must restart before invalid input");
    model.advance_middle_autoscroll(
        std::numeric_limits<float>::infinity(), 0.01f);
    expect(model.last_auto_scroll_cancel_reason()
            == mv::ComicAutoScrollCancelReason::InvalidInput,
        "non-finite pointer input must cancel middle autoscroll fail-closed");

    expect(model.start_cruise(),
        "cruise must restart before a precise manual cancellation");
    model.cancel_auto_scroll(mv::ComicAutoScrollCancelReason::MouseWheel);
    model.scroll_by(10.0f);
    model.page_down();
    expect(model.auto_scroll_owner() == mv::ComicAutoScrollOwner::None
            && model.last_auto_scroll_cancel_reason()
                == mv::ComicAutoScrollCancelReason::MouseWheel,
        "generic model scrolling after cancellation must preserve the precise reason");
}

void test_autoscroll_cancel_reason_matrix() {
    const auto make_reader = [] {
        mv::ComicReaderModel model;
        model.set_viewport({1000.0f, 500.0f, 1.0f});
        model.set_pages(make_pages(5));
        expect(model.enter(0), "cancel matrix reader must enter a book");
        return model;
    };

    enum class CancelAction {
        ToggleOff,
        Scrollbar,
        ExitMode,
        EmptyBook,
        InvalidInput,
        CruiseBoundary,
        MiddleBoundary,
    };
    struct CancelCase {
        CancelAction action;
        mv::ComicAutoScrollCancelReason expected;
    };
    constexpr CancelCase cancel_cases[] = {
        {CancelAction::ToggleOff, mv::ComicAutoScrollCancelReason::ToggleOff},
        {CancelAction::Scrollbar, mv::ComicAutoScrollCancelReason::Scrollbar},
        {CancelAction::ExitMode, mv::ComicAutoScrollCancelReason::ExitMode},
        {CancelAction::EmptyBook, mv::ComicAutoScrollCancelReason::EmptyBook},
        {CancelAction::InvalidInput, mv::ComicAutoScrollCancelReason::InvalidInput},
        {CancelAction::CruiseBoundary, mv::ComicAutoScrollCancelReason::Boundary},
        {CancelAction::MiddleBoundary, mv::ComicAutoScrollCancelReason::Boundary},
    };
    for (const CancelCase& cancel_case : cancel_cases) {
        mv::ComicReaderModel model = make_reader();
        switch (cancel_case.action) {
        case CancelAction::ToggleOff:
            expect(model.start_cruise(), "toggle cancellation requires active cruise");
            model.toggle_cruise();
            break;
        case CancelAction::Scrollbar:
            expect(model.start_cruise(), "scrollbar cancellation requires active cruise");
            model.set_scroll_from_scrollbar(1.0f);
            break;
        case CancelAction::ExitMode:
            expect(model.start_cruise(), "exit cancellation requires active cruise");
            model.exit_current_index();
            break;
        case CancelAction::EmptyBook:
            expect(model.start_cruise(), "empty-book cancellation requires active cruise");
            model.set_pages({});
            break;
        case CancelAction::InvalidInput:
            expect(model.start_middle_autoscroll(100.0f),
                "invalid-input cancellation requires active middle autoscroll");
            model.advance_middle_autoscroll(
                std::numeric_limits<float>::infinity(), 0.01f);
            break;
        case CancelAction::CruiseBoundary: {
            const float maximum = model.scroll_metrics().max_scroll;
            model.set_scroll_from_scrollbar(maximum - 1.0f);
            expect(model.start_cruise(),
                "cruise boundary cancellation requires active cruise");
            model.advance_cruise(0.1f);
            break;
        }
        case CancelAction::MiddleBoundary:
            expect(model.start_middle_autoscroll(100.0f),
                "middle boundary cancellation requires active middle autoscroll");
            model.advance_middle_autoscroll(0.0f, 0.05f);
            break;
        }
        expect(model.auto_scroll_owner() == mv::ComicAutoScrollOwner::None
                && model.last_auto_scroll_cancel_reason() == cancel_case.expected,
            "each active cancellation path must report its exact reason");
        model.scroll_by(1.0f);
        model.page_down();
        expect(model.last_auto_scroll_cancel_reason() == cancel_case.expected,
            "generic operations with no owner must not overwrite a precise reason");
    }

    constexpr mv::ComicAutoScrollCancelReason precise_reasons[] = {
        mv::ComicAutoScrollCancelReason::LeftButton,
        mv::ComicAutoScrollCancelReason::Escape,
        mv::ComicAutoScrollCancelReason::KeyboardPage,
        mv::ComicAutoScrollCancelReason::MouseWheel,
        mv::ComicAutoScrollCancelReason::FocusLost,
    };
    for (const mv::ComicAutoScrollCancelReason reason : precise_reasons) {
        mv::ComicReaderModel model = make_reader();
        expect(model.start_cruise(),
            "precise cancellation reason requires an active owner");
        model.cancel_auto_scroll(reason);
        model.scroll_by(1.0f);
        model.page_down();
        expect(model.auto_scroll_owner() == mv::ComicAutoScrollOwner::None
                && model.last_auto_scroll_cancel_reason() == reason,
            "precise input cancellation must survive later generic operations");
    }
}

void test_scroll_metrics_page_step_and_finite_overflow() {
    const mv::ComicScrollMetrics empty =
        mv::normalize_comic_scroll_metrics(0.0f, 600.0f, 0.0f);
    expect(empty.is_valid && !empty.scrollable()
            && empty.max_scroll == 0.0f && empty.scroll == 0.0f,
        "a zero canvas must normalize to a finite non-scrollable state");
    const mv::ComicScrollMetrics short_canvas =
        mv::normalize_comic_scroll_metrics(400.0f, 600.0f, 200.0f);
    expect(short_canvas.is_valid && !short_canvas.scrollable()
            && short_canvas.scroll == 0.0f,
        "a short canvas must clamp scroll to zero");
    const mv::ComicScrollMetrics long_canvas =
        mv::normalize_comic_scroll_metrics(1.0e30f, 1000.0f, 5.0e29f);
    expect(long_canvas.is_valid && long_canvas.scrollable()
            && std::isfinite(long_canvas.max_scroll)
            && std::isfinite(long_canvas.scroll),
        "a very long finite canvas must normalize without overflow");
    expect(!mv::normalize_comic_scroll_metrics(
        std::numeric_limits<float>::infinity(), 1.0f, 0.0f).is_valid,
        "infinite scroll metrics must fail closed");
    expect(!mv::normalize_comic_scroll_metrics(
        1000.0f, 500.0f,
        std::numeric_limits<float>::quiet_NaN()).is_valid,
        "NaN scroll metrics must fail closed");
    const float unsafe_finite = std::nextafter(
        mv::kComicMaxFiniteCoordinate,
        std::numeric_limits<float>::infinity());
    expect(std::isfinite(unsafe_finite)
            && !mv::normalize_comic_scroll_metrics(
                unsafe_finite, 500.0f, 0.0f).is_valid,
        "a finite coordinate just above the safety threshold must fail closed");
    expect(!mv::normalize_comic_scroll_metrics(-1.0f, 500.0f, 0.0f).is_valid
            && !mv::normalize_comic_scroll_metrics(
                1000.0f, -1.0f, 0.0f).is_valid,
        "negative total and viewport metrics must fail closed");

    mv::ComicReaderModel model;
    model.set_viewport({1000.0f, 500.0f, 1.0f});
    model.set_pages(make_pages(5));
    expect(model.enter(0), "scroll business model must enter a long canvas");
    model.set_scroll_from_scrollbar(-100.0f);
    expect_near(model.scroll(), 0.0f, 0.0f,
        "a mapped scroll below the canvas must clamp to zero");
    const float maximum = model.scroll_metrics().max_scroll;
    model.set_scroll_from_scrollbar(maximum + 1000.0f);
    expect_near(model.scroll(), maximum, 0.001f,
        "a mapped scroll above the canvas must clamp to max scroll");
    model.set_scroll_from_scrollbar(750.0f);
    model.scrollbar_page_step(mv::ComicScrollDirection::Forward);
    expect_near(model.scroll(), 1250.0f, 0.001f,
        "track-after business input must step exactly one viewport");
    model.scrollbar_page_step(mv::ComicScrollDirection::Backward);
    expect_near(model.scroll(), 750.0f, 0.001f,
        "track-before business input must step exactly one viewport");

    const float before_invalid = model.scroll();
    model.set_scroll_from_scrollbar(std::numeric_limits<float>::infinity());
    expect_near(model.scroll(), before_invalid, 0.0f,
        "a non-finite renderer mapping must leave model scroll unchanged");

    model.set_viewport({
        mv::kComicMaxFiniteCoordinate,
        mv::kComicMaxFiniteCoordinate,
        mv::kComicMaxFiniteCoordinate});
    model.set_pages({
        {L"overflow", 1, std::numeric_limits<std::uint32_t>::max(), false}});
    const auto page = model.geometry(0);
    expect(page.has_value() && std::isfinite(page->left)
            && std::isfinite(page->width) && std::isfinite(page->height)
            && std::isfinite(model.total_height()),
        "finite arithmetic overflow must saturate to finite model geometry");
}

void test_large_library_materializes_only_request_window() {
    mv::ComicReaderModel model;
    model.set_viewport({1000.0f, 500.0f, 1.0f});
    model.set_pages(make_pages(30000));
    expect(model.enter(15000), "the 30K model must enter at an arbitrary anchor");
    const mv::ComicPageRange requested = model.request_range();
    const auto geometries = model.materialize(requested);
    expect(requested.size() <= 4,
        "the directional request window must stay viewport-bounded at 30K pages");
    expect(static_cast<int>(geometries.size()) == requested.size(),
        "materialization must create geometry only for the request window");
    expect(requested.first > 0 && requested.last < 30000,
        "the request window must not expand to the full virtual canvas");
}

void test_lru_obeys_comic_and_application_soft_limits() {
    constexpr std::size_t mib = 1024ULL * 1024ULL;
    mv::ComicLruBudget lru;
    lru.touch(0, 100 * mib);
    lru.touch(1, 100 * mib);
    lru.touch(2, 100 * mib);
    const auto first_eviction = lru.evict_to_budget(300 * mib, {1, 2});
    expect(first_eviction == std::vector<int>{0},
        "LRU must evict the oldest non-visible page under the overall 512 MiB limit");
    expect(lru.contains(1) && lru.contains(2) && lru.resident_bytes() == 200 * mib,
        "the visible page must remain resident while the cache reaches its budget");

    lru.touch(3, 80 * mib);
    const auto second_eviction = lru.evict_to_budget(400 * mib, {1, 2});
    expect(second_eviction == std::vector<int>{2, 3},
        "LRU ordering must bound non-visible pages as application memory grows");
    expect(lru.contains(1) && lru.resident_bytes() == 100 * mib,
        "a protected visible page may remain under the soft-limit contract");
    expect(lru.allowed_bytes(400 * mib) == 112 * mib,
        "comic cache allowance must be capped by remaining application budget");

    lru.erase(1);
    expect(!lru.contains(1) && lru.resident_bytes() == 0,
        "erasing a decoded page must release its accounted cache bytes");
    lru.touch(4, 20 * mib);
    lru.clear();
    expect(!lru.contains(4) && lru.resident_bytes() == 0,
        "generation changes must clear all comic cache accounting");
}

void test_production_app_binds_virtual_window_and_lru() {
    const std::filesystem::path source_root(MINVIEW_SOURCE_DIR);
    const std::string app = read_source(source_root / "src" / "app.cpp");
    const auto require_binding = [&app](const char* text, const char* message) {
        expect(app.find(text) != std::string::npos, message);
    };

    require_binding("m_comic_reader.request_range()",
        "production loading must use the model's directional request window");
    require_binding("m_comic_loader.replace_requests(std::move(loads))",
        "production loading must replace work with the bounded request set");
    require_binding("result.generation != m_comic_generation",
        "production result handling must reject stale generations");
    require_binding("m_comic_lru.evict_to_budget(",
        "production cache trimming must enforce the shared soft limit");
    require_binding("m_comic_reader.materialize(visible)",
        "production rendering must materialize only visible page geometry");
    require_binding("m_renderer.draw_comic_pages(draw_items,",
        "production rendering must consume the tested stacked renderer contract");
}

void test_production_toolbar_forwards_comic_commands() {
    const std::filesystem::path source_root(MINVIEW_SOURCE_DIR);
    const std::string app = read_source(source_root / "src" / "app.cpp");
    const std::size_t toolbar_begin =
        app.find("void App::show_toolbar_menu(HWND hwnd, int idx, int x, int y)");
    const std::size_t toolbar_end =
        app.find("void App::open_in_explorer()", toolbar_begin);
    expect(toolbar_begin != std::string::npos && toolbar_end != std::string::npos,
        "production toolbar dispatch must remain discoverable");

    const std::string toolbar =
        app.substr(toolbar_begin, toolbar_end - toolbar_begin);
    const std::size_t dispatch = toolbar.find("switch (cmd)");
    const std::size_t comic = toolbar.find("case IDM_COMIC:", dispatch);
    const std::size_t seamless =
        toolbar.find("case IDM_COMIC_SEAMLESS:", dispatch);
    const std::size_t forward =
        toolbar.find("SendMessageW(hwnd, WM_COMMAND, cmd, 0)", dispatch);
    expect(toolbar.find("TPM_RETURNCMD | TPM_NONOTIFY") != std::string::npos,
        "toolbar regression must exercise the manual command forwarding path");
    expect(dispatch != std::string::npos && forward != std::string::npos,
        "toolbar commands must be forwarded through WM_COMMAND");
    expect(comic < forward,
        "the comic mode toolbar command must reach WM_COMMAND");
    expect(seamless < forward,
        "the seamless toolbar command must reach WM_COMMAND");
}

} // namespace

int main() {
    try {
        test_width_gap_and_failure_geometry();
        test_anchor_survives_width_viewport_and_reordering();
        test_removed_anchor_selects_successor_and_empty_disables();
        test_scroll_commands_and_directional_request_window();
        test_decode_update_preserves_anchor();
        test_page_status_and_change_event();
        test_cruise_speed_elapsed_cap_and_boundaries();
        test_autoscroll_ownership_and_middle_curve();
        test_autoscroll_cancel_reason_matrix();
        test_scroll_metrics_page_step_and_finite_overflow();
        test_large_library_materializes_only_request_window();
        test_lru_obeys_comic_and_application_soft_limits();
        test_production_app_binds_virtual_window_and_lru();
        test_production_toolbar_forwards_comic_commands();
        std::cout << "comic_reader_model_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "comic_reader_model_tests: FAIL: " << error.what() << '\n';
        return 1;
    }
}
