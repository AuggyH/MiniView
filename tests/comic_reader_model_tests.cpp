#include "comic_reader_model.h"

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void expect_near(float actual, float expected, float tolerance, const char* message) {
    if (std::fabs(actual - expected) > tolerance) throw std::runtime_error(message);
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
}

} // namespace

int main() {
    try {
        test_width_gap_and_failure_geometry();
        test_anchor_survives_width_viewport_and_reordering();
        test_removed_anchor_selects_successor_and_empty_disables();
        test_scroll_commands_and_directional_request_window();
        test_decode_update_preserves_anchor();
        test_large_library_materializes_only_request_window();
        test_lru_obeys_comic_and_application_soft_limits();
        test_production_app_binds_virtual_window_and_lru();
        std::cout << "comic_reader_model_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "comic_reader_model_tests: FAIL: " << error.what() << '\n';
        return 1;
    }
}
