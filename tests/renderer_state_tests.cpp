#include "renderer_state.h"

#include <cmath>
#include <d2d1.h>
#include <dxgi.h>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

static_assert(std::is_same_v<
    decltype(mv::ComicTextOverlayLayout{}.text), std::wstring_view>);
static_assert(std::is_same_v<
    decltype(mv::ComicControlsRenderInput{}.page_badge_text),
    std::wstring_view>);
static_assert(std::is_same_v<
    decltype(mv::ComicControlsRenderInput{}.transient_text),
    std::wstring_view>);

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

bool nearly_equal(float left, float right, float tolerance = 0.01f) {
    return std::abs(left - right) <= tolerance;
}

void expect_near(float actual, float expected, const char* message) {
    if (std::fabs(actual - expected) > 0.01f) {
        std::cerr << "FAIL: " << message << " (expected " << expected
                  << ", got " << actual << ")\n";
        ++failures;
    }
}

void test_transition_ease() {
    expect_near(mv::transition_ease(0.0f), 0.0f, "ease(0) must be 0");
    expect_near(mv::transition_ease(1.0f), 1.0f, "ease(1) must be 1");
    expect_near(mv::transition_ease(0.25f), 0.129162f,
        "CSS ease-in-out must be 12.92% at 25% (Quick Look affine fit)");
    expect_near(mv::transition_ease(0.5f), 0.5f,
        "CSS ease-in-out must be 50% at midpoint");
    expect_near(mv::transition_ease(0.75f), 0.870838f,
        "CSS ease-in-out must be 87.08% at 75%");
    expect_near(mv::transition_ease(-0.5f), 0.0f, "ease must clamp below 0");
    expect_near(mv::transition_ease(1.5f), 1.0f, "ease must clamp above 1");
    expect(mv::transition_ease(0.2f) < mv::transition_ease(0.4f)
            && mv::transition_ease(0.4f) < mv::transition_ease(0.6f)
            && mv::transition_ease(0.6f) < mv::transition_ease(0.8f),
        "ease must be monotonic on [0, 1]");
    for (const float s : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f, 0.37f}) {
        expect_near(mv::transition_ease(s),
            1.0f - mv::transition_ease(1.0f - s),
            "CSS ease-in-out must satisfy E(s)=1-E(1-s)");
    }
}

void test_device_loss_detection() {
    expect(!mv::should_recreate_render_device(S_OK),
        "success must not recreate the render device");
    expect(!mv::should_recreate_render_device(E_ACCESSDENIED),
        "an ordinary HRESULT failure must not tear down the device");
    expect(!mv::should_recreate_render_device(E_FAIL),
        "E_FAIL must not tear down the device");
    expect(mv::should_recreate_render_device(DXGI_ERROR_DEVICE_REMOVED),
        "device removed must recreate");
    expect(mv::should_recreate_render_device(DXGI_ERROR_DEVICE_RESET),
        "device reset must recreate");
    expect(mv::should_recreate_render_device(DXGI_ERROR_DEVICE_HUNG),
        "device hung must recreate");
    expect(mv::should_recreate_render_device(D2DERR_RECREATE_TARGET),
        "recreate-target must recreate");
}

void test_transition_ease_exit() {
    // Exit is the exact time-mirror: same CSS ease-in-out curve.
    expect_near(mv::transition_ease_exit(0.0f), 0.0f, "exit ease(0) must be 0");
    expect_near(mv::transition_ease_exit(1.0f), 1.0f, "exit ease(1) must be 1");
    expect_near(mv::transition_ease_exit(0.25f), 0.129162f,
        "exit ease must be 12.92% at 25% (mirror of entry)");
    expect_near(mv::transition_ease_exit(0.5f), 0.5f,
        "exit ease must be 50% at midpoint");
    expect_near(mv::transition_ease_exit(0.75f), 0.870838f,
        "exit ease must be 87.08% at 75%");
    expect_near(mv::transition_ease_exit(-0.5f), 0.0f,
        "exit ease must clamp below 0");
    expect_near(mv::transition_ease_exit(1.5f), 1.0f,
        "exit ease must clamp above 1");
    expect(mv::transition_ease_exit(0.2f) < mv::transition_ease_exit(0.5f)
            && mv::transition_ease_exit(0.5f) < mv::transition_ease_exit(0.8f),
        "exit ease must be monotonic");
}

void test_transition_veil_exit() {
    // Background crossfade uses the same FLIP curve as geometry.
    expect_near(mv::transition_veil_exit(0.0f), 0.0f, "veil(0) must be 0");
    expect_near(mv::transition_veil_exit(0.25f), 0.129162f,
        "veil must be 12.92% at 25%");
    expect_near(mv::transition_veil_exit(0.5f), 0.5f, "veil must be 0.5 at 50%");
    expect_near(mv::transition_veil_exit(0.75f), 0.870838f,
        "veil must be 87.08% at 75%");
    expect_near(mv::transition_veil_exit(1.0f), 1.0f, "veil(1) must be 1");
    expect_near(mv::transition_veil_exit(-0.5f), 0.0f,
        "veil must clamp below 0");
    expect(mv::transition_veil_exit(0.1f) < mv::transition_veil_exit(0.5f)
            && mv::transition_veil_exit(0.5f) < mv::transition_veil_exit(0.9f),
        "veil must be monotonic");
}

std::vector<mv::ComicPageRenderInput> make_render_inputs(
    const std::vector<mv::ComicPageGeometry>& geometries,
    mv::ComicPageVisual visual = mv::ComicPageVisual::Bitmap) {
    std::vector<mv::ComicPageRenderInput> inputs;
    inputs.reserve(geometries.size());
    for (const mv::ComicPageGeometry& geometry : geometries) {
        inputs.push_back({geometry, visual});
    }
    return inputs;
}

void test_dpi_metrics() {
    const mv::ComicRenderMetrics dpi96 =
        mv::comic_render_metrics(96.0f, false);
    expect_near(dpi96.dpi_scale, 1.0f, "96 DPI scale must be 1");
    expect_near(dpi96.page_gap, 12.0f, "96 DPI page gap must be 12 px");
    expect_near(dpi96.corner_radius, 4.0f, "96 DPI radius must be 4 px");
    expect_near(dpi96.error_font_size, 14.0f,
        "96 DPI error text must retain its logical size");

    const mv::ComicRenderMetrics dpi168 =
        mv::comic_render_metrics(168.0f, false);
    expect_near(dpi168.dpi_scale, 1.75f, "168 DPI scale must be 1.75");
    expect_near(dpi168.page_gap, 21.0f, "12 DIP must become 21 px at 168 DPI");
    expect_near(dpi168.corner_radius, 7.0f, "radius must scale at 168 DPI");
    expect_near(dpi168.card_padding, 28.0f, "card padding must scale at 168 DPI");
    expect_near(dpi168.error_font_size, 24.5f,
        "error text must scale at 168 DPI");

    const mv::ComicRenderMetrics dpi192 =
        mv::comic_render_metrics(192.0f, true);
    expect_near(dpi192.dpi_scale, 2.0f, "192 DPI scale must be 2");
    expect_near(dpi192.page_gap, 0.0f, "seamless pages must have no gap");
    expect_near(dpi192.corner_radius, 0.0f,
        "seamless pages must not expose rounded background seams");
    expect_near(dpi192.card_border_width, 2.0f,
        "card border must scale at 192 DPI");
    expect_near(dpi192.error_font_size, 28.0f,
        "error text must scale at 192 DPI");
}

void test_narrow_viewport_and_error_card() {
    mv::ComicReaderModel model;
    model.set_viewport({320.0f, 400.0f, 1.0f});
    model.set_pages({
        {L"first", 100, 100, false},
        {L"broken", 100, 200, true},
        {L"last", 100, 100, false}});
    expect(model.enter(0), "narrow comic fixture must enter at the first page");

    const auto geometries = model.materialize({0, 3});
    expect(geometries.size() == 3,
        "model must expose all requested geometries without a stitched bitmap");
    expect_near(geometries[0].width, 294.4f,
        "narrow viewport must use 92 percent target width");
    expect_near(geometries[0].left, 12.8f,
        "narrow page must remain horizontally centered");
    expect_near(
        geometries[1].top - geometries[0].top - geometries[0].height,
        12.0f, "model and renderer must agree on the 12 DIP page gap");

    const auto inputs = make_render_inputs(geometries);
    const mv::ComicRenderPlan plan = mv::build_comic_render_plan(
        inputs, {0.0f, 40.0f, 320.0f, 400.0f, 0.0f, 96.0f, false});
    expect(plan.pages.size() == 2,
        "renderer plan must cull pages beyond the narrow viewport");
    expect_near(plan.pages[0].destination.top, 40.0f,
        "content top is a pixel offset and must not be DPI-scaled again");
    expect_near(plan.pages[1].clip.bottom, 440.0f,
        "second page must be clipped at the viewport bottom");
    expect(plan.pages[1].visual == mv::ComicPageVisual::Error,
        "decode failure from model geometry must promote an error card");

    const mv::ComicRenderPlan taller = mv::build_comic_render_plan(
        inputs, {0.0f, 0.0f, 320.0f, 1000.0f, 0.0f, 96.0f, false});
    expect(taller.pages.size() == 3,
        "one failed page must not stop later pages from rendering");
    expect(taller.pages[1].visual == mv::ComicPageVisual::Error,
        "failed page must keep its position in a continuous stack");
    expect(taller.pages[2].visual == mv::ComicPageVisual::Bitmap,
        "page after an error card must retain its bitmap visual");
}

void test_wide_viewport_and_seamless_gap() {
    mv::ComicReaderModel model;
    model.set_viewport({3000.0f, 500.0f, 2.0f});
    model.set_pages({
        {L"wide-a", 100, 100, false},
        {L"wide-b", 100, 100, false}});
    model.set_seamless(true);
    expect(model.enter(0), "wide comic fixture must enter at the first page");

    const auto geometries = model.materialize({0, 2});
    expect_near(geometries[0].width, 2400.0f,
        "wide viewport must honor the 1200 DIP cap at 192 DPI");
    expect_near(geometries[0].left, 300.0f,
        "capped wide page must remain centered");
    expect_near(
        geometries[1].top - geometries[0].top - geometries[0].height,
        0.0f, "seamless model pages must be adjacent");

    const auto inputs = make_render_inputs(geometries);
    const mv::ComicRenderPlan plan = mv::build_comic_render_plan(
        inputs,
        {0.0f, 80.0f, 3000.0f, 500.0f, 2400.0f, 192.0f, true});
    expect(plan.pages.size() == 1,
        "page ending exactly at the top boundary must be culled");
    expect(plan.pages[0].page_index == 1,
        "the following seamless page must start at the boundary");
    expect_near(plan.pages[0].destination.top, 80.0f,
        "wide viewport content offset must remain in physical pixels");
    expect_near(plan.metrics.page_gap, 0.0f,
        "wide seamless render plan must preserve the 0 DIP gap");
}

void test_clip_boundaries() {
    const std::vector<mv::ComicPageRenderInput> inputs = {
        {{0, 0.0f, -100.0f, 500.0f, 100.0f, false},
            mv::ComicPageVisual::Bitmap},
        {{1, 0.0f, -50.0f, 500.0f, 100.0f, false},
            mv::ComicPageVisual::Bitmap},
        {{2, 0.0f, 250.0f, 500.0f, 100.0f, false},
            mv::ComicPageVisual::Error},
        {{3, 0.0f, 300.0f, 500.0f, 100.0f, false},
            mv::ComicPageVisual::Bitmap}};
    const mv::ComicRenderPlan plan = mv::build_comic_render_plan(
        inputs, {0.0f, 100.0f, 500.0f, 300.0f, 0.0f, 168.0f, false});

    expect(plan.pages.size() == 2,
        "pages touching only the top or bottom edge must not be drawn");
    expect(plan.pages[0].page_index == 1 && plan.pages[1].page_index == 2,
        "only pages with positive-area viewport intersections may render");
    expect_near(plan.pages[0].destination.top, 50.0f,
        "top-clipped page must retain its full destination");
    expect_near(plan.pages[0].clip.top, 100.0f,
        "top-clipped page must expose the viewport clip boundary");
    expect_near(plan.pages[1].clip.bottom, 400.0f,
        "bottom-clipped page must expose the viewport clip boundary");
    expect(plan.pages[1].visual == mv::ComicPageVisual::Error,
        "explicit error visual must survive clipping");
    expect_near(plan.metrics.page_gap, 21.0f,
        "168 DPI render plan must keep a 21 px background gap");
}

void test_non_finite_geometry_fails_closed() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const float maximum = std::numeric_limits<float>::max();
    const std::vector<mv::ComicPageRenderInput> valid_page = {
        {{0, 0.0f, 0.0f, 100.0f, 100.0f, false},
            mv::ComicPageVisual::Bitmap}};

    const auto expect_empty_viewport = [&valid_page](
        mv::ComicRenderViewport viewport, const char* message) {
        const mv::ComicRenderPlan plan =
            mv::build_comic_render_plan(valid_page, viewport);
        expect(plan.viewport.empty() && plan.pages.empty(), message);
    };
    expect_empty_viewport(
        {nan, 0.0f, 100.0f, 100.0f, 0.0f, 96.0f, false},
        "NaN viewport left must fail closed");
    expect_empty_viewport(
        {0.0f, infinity, 100.0f, 100.0f, 0.0f, 96.0f, false},
        "positive-infinite viewport top must fail closed");
    expect_empty_viewport(
        {-infinity, 0.0f, 100.0f, 100.0f, 0.0f, 96.0f, false},
        "negative-infinite viewport left must fail closed");
    expect_empty_viewport(
        {0.0f, 0.0f, infinity, 100.0f, 0.0f, 96.0f, false},
        "infinite viewport width must fail closed");
    expect_empty_viewport(
        {0.0f, 0.0f, 100.0f, 100.0f, nan, 96.0f, false},
        "NaN scroll offset must fail closed");
    expect_empty_viewport(
        {0.0f, 0.0f, 100.0f, 100.0f, -infinity, 96.0f, false},
        "negative-infinite scroll offset must fail closed");
    expect_empty_viewport(
        {maximum, 0.0f, maximum, 100.0f, 0.0f, 96.0f, false},
        "finite viewport right overflow must fail closed");
    expect_empty_viewport(
        {0.0f, maximum, 100.0f, maximum, 0.0f, 96.0f, false},
        "finite viewport bottom overflow must fail closed");

    expect(mv::ComicRenderRect{nan, 0.0f, 1.0f, 1.0f}.empty(),
        "rectangles with NaN boundaries must be empty");
    expect(mv::ComicRenderRect{0.0f, 0.0f, infinity, 1.0f}.empty(),
        "rectangles with infinite boundaries must be empty");
    expect(mv::ComicRenderRect{-maximum, 0.0f, maximum, 1.0f}.empty(),
        "rectangles with overflowing derived width must be empty");

    const std::vector<mv::ComicPageRenderInput> overflowing_pages = {
        {{0, maximum, 0.0f, maximum, 100.0f, false},
            mv::ComicPageVisual::Bitmap},
        {{1, 0.0f, maximum, 100.0f, maximum, false},
            mv::ComicPageVisual::Error}};
    const mv::ComicRenderPlan page_overflow = mv::build_comic_render_plan(
        overflowing_pages,
        {0.0f, 0.0f, 500.0f, 500.0f, 0.0f, 96.0f, false});
    expect(page_overflow.pages.empty(),
        "derived page boundary overflow must be culled before D2D");

    const float large = maximum / 4.0f;
    const std::vector<mv::ComicPageRenderInput> large_page = {
        {{0, 0.0f, 0.0f, large, 100.0f, false},
            mv::ComicPageVisual::Bitmap}};
    const mv::ComicRenderPlan finite_large = mv::build_comic_render_plan(
        large_page,
        {large, 0.0f, large, 100.0f, 0.0f, 96.0f, false});
    expect(finite_large.pages.size() == 1,
        "large finite coordinates with finite derived bounds must remain valid");
}

void test_comic_scrollbar_dpi_and_boundaries() {
    struct DpiCase { float dpi; float scale; };
    const DpiCase cases[] = {
        {96.0f, 1.0f}, {168.0f, 1.75f}, {192.0f, 2.0f}};
    for (const DpiCase& test : cases) {
        const mv::ComicRenderRect viewport{
            0.0f, 40.0f * test.scale,
            400.0f * test.scale, 840.0f * test.scale};
        const float virtual_height = 80000.0f * test.scale;
        const mv::ComicScrollbarGeometry start =
            mv::build_comic_scrollbar_geometry(
                viewport, virtual_height, 0.0f, test.dpi, true, false);
        expect(start.visible, "scrollbar must be visible when canvas exceeds viewport");
        expect(start.hovered && !start.dragging,
            "scrollbar visual state must preserve hover without drag");
        expect_near(start.thumb.height(), 32.0f * test.scale,
            "scrollbar thumb minimum must scale from 32 DIP");
        expect_near(start.thumb.top, start.track.top,
            "zero scroll must pin thumb to track start");

        const float scroll_range = virtual_height - viewport.height();
        const mv::ComicScrollbarGeometry end =
            mv::build_comic_scrollbar_geometry(
                viewport, virtual_height, scroll_range,
                test.dpi, false, true);
        expect(end.visible && end.dragging && !end.hovered,
            "scrollbar visual state must preserve drag without hover");
        expect_near(end.thumb.bottom, end.track.bottom,
            "maximum scroll must pin thumb to track end");
        expect_near(end.scroll_range, scroll_range,
            "scrollbar must expose the virtual scroll range");
    }
}

void test_comic_scrollbar_hit_test_and_drag() {
    const mv::ComicScrollbarGeometry geometry =
        mv::build_comic_scrollbar_geometry(
            {0.0f, 40.0f, 1000.0f, 840.0f},
            4000.0f, 800.0f, 96.0f);
    expect(geometry.visible, "hit-test fixture must have a scrollbar");
    const float hit_x = (geometry.hit_bounds.left
        + geometry.hit_bounds.right) * 0.5f;
    expect(mv::hit_test_comic_scrollbar(
            geometry, hit_x, (geometry.thumb.top + geometry.thumb.bottom) * 0.5f)
            == mv::ComicScrollbarHit::Thumb,
        "thumb bounds must hit the draggable thumb");
    expect(mv::hit_test_comic_scrollbar(
            geometry, hit_x, geometry.thumb.top - 1.0f)
            == mv::ComicScrollbarHit::PageBackward,
        "track above thumb must request one viewport backward");
    expect(mv::hit_test_comic_scrollbar(
            geometry, hit_x, geometry.thumb.bottom + 1.0f)
            == mv::ComicScrollbarHit::PageForward,
        "track below thumb must request one viewport forward");
    expect(mv::hit_test_comic_scrollbar(
            geometry, geometry.hit_bounds.left - 1.0f, geometry.thumb.top)
            == mv::ComicScrollbarHit::None,
        "coordinates outside the hit zone must not affect scrolling");
    expect(mv::hit_test_comic_scrollbar(
            geometry, hit_x, geometry.thumb.top)
            == mv::ComicScrollbarHit::Thumb
            && mv::hit_test_comic_scrollbar(
                geometry, hit_x, geometry.thumb.bottom)
                == mv::ComicScrollbarHit::Thumb,
        "half-open outer bounds must preserve exact thumb-edge semantics");

    const float grab_offset = geometry.thumb.height() * 0.5f;
    const mv::ComicScrollbarDragResult start =
        mv::map_comic_scrollbar_drag(
            geometry, geometry.track.top + grab_offset, grab_offset);
    expect(start.valid, "finite thumb drag must produce a scroll mapping");
    expect_near(start.scroll_y, 0.0f,
        "dragging to track start must map to zero scroll");
    const mv::ComicScrollbarDragResult middle =
        mv::map_comic_scrollbar_drag(
            geometry, geometry.track.top + geometry.thumb_travel * 0.5f
                + grab_offset, grab_offset);
    expect(middle.valid, "middle thumb drag must remain valid");
    expect_near(middle.scroll_y, geometry.scroll_range * 0.5f,
        "middle thumb position must map to middle virtual scroll");
    const mv::ComicScrollbarDragResult end =
        mv::map_comic_scrollbar_drag(
            geometry, geometry.track.bottom, grab_offset);
    expect(end.valid, "drag past track end must clamp rather than fail");
    expect_near(end.scroll_y, geometry.scroll_range,
        "drag past track end must clamp to maximum scroll");
}

void test_comic_scrollbar_half_open_boundaries() {
    const float dpis[] = {96.0f, 168.0f, 192.0f};
    for (const float dpi : dpis) {
        const float scale = dpi / 96.0f;
        const mv::ComicRenderRect content_viewport{
            0.0f, 40.0f * scale, 400.0f * scale, 840.0f * scale};
        const mv::ComicScrollbarGeometry geometry =
            mv::build_comic_scrollbar_geometry(
                content_viewport, 8000.0f * scale,
                2000.0f * scale, dpi);
        expect(geometry.visible,
            "half-open boundary fixture must expose a scrollbar");
        const float thumb_y = (geometry.thumb.top + geometry.thumb.bottom) * 0.5f;
        expect(mv::hit_test_comic_scrollbar(
                geometry, geometry.hit_bounds.right, thumb_y)
                == mv::ComicScrollbarHit::None,
            "content right edge/panel left edge must be outside scrollbar hit bounds");
        expect(mv::hit_test_comic_scrollbar(
                geometry, geometry.hit_bounds.right - 1.0f, thumb_y)
                == mv::ComicScrollbarHit::Thumb,
            "the adjacent physical pixel inside the right edge must remain hittable");
        expect(mv::hit_test_comic_scrollbar(
                geometry, geometry.hit_bounds.left, geometry.hit_bounds.bottom)
                == mv::ComicScrollbarHit::None,
            "the exact bottom edge must be outside scrollbar hit bounds");
        expect(mv::hit_test_comic_scrollbar(
                geometry, geometry.hit_bounds.left,
                geometry.hit_bounds.bottom - 1.0f)
                == mv::ComicScrollbarHit::PageForward,
            "the adjacent physical pixel inside the bottom edge must retain track semantics");
        expect(mv::hit_test_comic_scrollbar(
                geometry, geometry.hit_bounds.left, geometry.hit_bounds.top)
                != mv::ComicScrollbarHit::None,
            "the closed top and left edges must remain inside hit bounds");
    }
}

void test_comic_controls_viewport_avoidance() {
    mv::ComicControlsRenderInput input;
    input.content_viewport = {0.0f, 40.0f, 920.0f, 800.0f};
    input.dpi = 96.0f;
    input.virtual_height = 4000.0f;
    input.scroll_y = 800.0f;
    input.anchored_page_index = 11;
    input.total_pages = 86;
    const std::wstring page_badge = L"12 / 86";
    input.page_badge_text = page_badge;
    const mv::ComicControlsLayout panel =
        mv::build_comic_controls_layout(input);
    expect(panel.scrollbar.visible && panel.page_badge.visible,
        "panel-open viewport must retain scrollbar and progress badge");
    expect(panel.scrollbar.hit_bounds.right <= input.content_viewport.right,
        "scrollbar must remain on the content side of an open panel");
    expect(panel.page_badge.bounds.right
            <= panel.scrollbar.hit_bounds.left - panel.metrics.overlay_gap,
        "page badge must avoid the scrollbar hit zone");
    expect(panel.page_badge.text == L"12 / 86",
        "progress badge must consume App-preformatted page feedback");
    expect(panel.page_badge.text.data() == page_badge.data(),
        "progress badge layout must borrow rather than copy its text");
    expect(!panel.transient_overlay.visible,
        "transient overlay must be hidden in the default state");

    input.content_viewport.right = 1200.0f;
    const mv::ComicControlsLayout collapsed =
        mv::build_comic_controls_layout(input);
    expect_near(
        collapsed.scrollbar.hit_bounds.right - panel.scrollbar.hit_bounds.right,
        280.0f, "closing a 280 px panel must move controls to content edge");

    input.content_viewport = {0.0f, 0.0f, 1200.0f, 900.0f};
    const mv::ComicControlsLayout fullscreen =
        mv::build_comic_controls_layout(input);
    expect_near(fullscreen.scrollbar.track.top, 8.0f,
        "fullscreen content starting at zero must only apply edge margin");
    expect_near(fullscreen.scrollbar.track.bottom, 892.0f,
        "fullscreen scrollbar must avoid the bottom edge margin");
}

void test_comic_controls_transient_layout() {
    mv::ComicControlsRenderInput input;
    input.content_viewport = {0.0f, 0.0f, 180.0f, 300.0f};
    input.dpi = 192.0f;
    input.virtual_height = 3000.0f;
    input.scroll_y = 1000.0f;
    input.anchored_page_index = 11;
    input.total_pages = 86;
    const std::wstring page_badge = L"12 / 86";
    std::wstring long_toast(1024, L'\u957F');
    long_toast += L" \u00B7 \u7B2C12/86\u9875";
    input.page_badge_text = page_badge;
    input.transient_text = long_toast;
    input.transient_kind = mv::ComicTransientOverlayKind::PageChange;
    const mv::ComicControlsLayout narrow =
        mv::build_comic_controls_layout(input);
    expect(narrow.page_badge.visible && narrow.transient_overlay.visible,
        "narrow 192 DPI viewport must retain both page overlays");
    expect(narrow.page_badge.bounds.left >= input.content_viewport.left,
        "narrow progress badge must stay inside the content viewport");
    expect(narrow.transient_overlay.bounds.left >= input.content_viewport.left
            && narrow.transient_overlay.bounds.right
                <= narrow.scrollbar.hit_bounds.left,
        "long filename toast must be bounded before scrollbar trimming");
    expect(narrow.transient_overlay.bounds.width()
            <= narrow.metrics.page_toast_max_width,
        "long filename toast must never exceed its DPI-aware maximum width");
    expect(narrow.transient_overlay.text.ends_with(L"\u00B7 \u7B2C12/86\u9875"),
        "page-change toast must consume exact App-preformatted feedback");
    expect(narrow.page_badge.text.data() == page_badge.data()
            && narrow.transient_overlay.text.data() == long_toast.data(),
        "overlay layouts must borrow both App-owned backing strings");

    input.transient_kind = mv::ComicTransientOverlayKind::None;
    expect(!mv::build_comic_controls_layout(input).transient_overlay.visible,
        "explicitly hidden transient state must draw no toast");

    input.transient_kind = mv::ComicTransientOverlayKind::Status;
    input.transient_text = L"\u81EA\u52A8\u6EDA\u52A8 \u00B7 1.0x";
    const mv::ComicControlsLayout status =
        mv::build_comic_controls_layout(input);
    expect(status.transient_overlay.visible
            && status.transient_overlay.text == L"\u81EA\u52A8\u6EDA\u52A8 \u00B7 1.0x",
        "status transient must display App-provided playback feedback");

    const std::wstring oversized(
        mv::kComicOverlayMaxTextCharacters + 1, L'x');
    input.transient_text = oversized;
    expect(!mv::build_comic_controls_layout(input).transient_overlay.visible,
        "oversized transient input must fail closed before allocation/rendering");
    input.transient_kind = mv::ComicTransientOverlayKind::None;
    input.page_badge_text = oversized;
    expect(!mv::build_comic_controls_layout(input).page_badge.visible,
        "oversized page badge input must fail closed before rendering");
}

void test_comic_autoscroll_graphic() {
    mv::ComicControlsRenderInput input;
    input.content_viewport = {0.0f, 40.0f, 800.0f, 840.0f};
    input.dpi = 168.0f;
    input.virtual_height = 4000.0f;
    input.scroll_y = 800.0f;
    input.anchored_page_index = 3;
    input.total_pages = 20;
    input.middle_autoscroll_active = true;
    input.autoscroll_anchor_x = 400.0f;
    input.autoscroll_anchor_y = 400.0f;
    input.autoscroll_pointer_x = 400.0f;
    const float dead_zone = 16.0f * 1.75f;

    input.autoscroll_pointer_y = input.autoscroll_anchor_y + dead_zone - 1.0f;
    const mv::ComicControlsLayout stationary =
        mv::build_comic_controls_layout(input);
    expect(stationary.autoscroll.visible
            && stationary.autoscroll.direction
                == mv::ComicAutoscrollDirection::Stationary,
        "pointer inside DPI-aware dead zone must remain stationary");
    expect_near(stationary.autoscroll.dead_zone_radius, dead_zone,
        "autoscroll dead zone must scale at 168 DPI");

    input.autoscroll_pointer_y = input.autoscroll_anchor_y + 240.0f;
    const mv::ComicControlsLayout forward =
        mv::build_comic_controls_layout(input);
    expect(forward.autoscroll.direction
            == mv::ComicAutoscrollDirection::Forward,
        "pointer below anchor must produce a forward direction graphic");
    expect(forward.autoscroll.arrow_tip_y > forward.autoscroll.arrow_tail_y
            && forward.autoscroll.arrow_head > 0.0f
            && forward.autoscroll.intensity > 0.0f,
        "forward autoscroll must expose an arrow and speed intensity");

    input.autoscroll_pointer_y = input.autoscroll_anchor_y - 240.0f;
    const mv::ComicControlsLayout backward =
        mv::build_comic_controls_layout(input);
    expect(backward.autoscroll.direction
            == mv::ComicAutoscrollDirection::Backward,
        "pointer above anchor must produce a backward direction graphic");
    expect(backward.autoscroll.arrow_tip_y < backward.autoscroll.arrow_tail_y,
        "backward autoscroll arrow must point upward");

    input.autoscroll_anchor_x = input.content_viewport.left + 1.0f;
    input.autoscroll_anchor_y = 400.0f;
    input.autoscroll_pointer_y = input.autoscroll_anchor_y + 240.0f;
    const mv::ComicControlsLayout near_left =
        mv::build_comic_controls_layout(input);
    expect(near_left.autoscroll.visible
            && near_left.autoscroll.direction
                == mv::ComicAutoscrollDirection::Forward
            && std::isfinite(near_left.autoscroll.arrow_tip_y),
        "anchor center near left edge must remain visible and directional");
    expect(near_left.autoscroll.anchor_x
            - near_left.autoscroll.dead_zone_radius
            < near_left.viewport.left,
        "near-left graphic must rely on the existing viewport clip");

    input.autoscroll_anchor_x = 400.0f;
    input.autoscroll_anchor_y = input.content_viewport.top + 1.0f;
    input.autoscroll_pointer_y = input.autoscroll_anchor_y + 240.0f;
    const mv::ComicControlsLayout near_top =
        mv::build_comic_controls_layout(input);
    expect(near_top.autoscroll.visible
            && near_top.autoscroll.direction
                == mv::ComicAutoscrollDirection::Forward
            && std::isfinite(near_top.autoscroll.arrow_tip_y),
        "anchor center near top edge must remain visible and directional");
    expect(near_top.autoscroll.anchor_y
            - near_top.autoscroll.dead_zone_radius
            < near_top.viewport.top,
        "near-top graphic must rely on the existing viewport clip");

    input.autoscroll_anchor_y = input.content_viewport.bottom - 1.0f;
    input.autoscroll_pointer_y = input.autoscroll_anchor_y - 240.0f;
    const mv::ComicControlsLayout near_bottom =
        mv::build_comic_controls_layout(input);
    expect(near_bottom.autoscroll.visible
            && near_bottom.autoscroll.direction
                == mv::ComicAutoscrollDirection::Backward
            && std::isfinite(near_bottom.autoscroll.arrow_tip_y),
        "anchor center near bottom edge must remain visible and directional");
    expect(near_bottom.autoscroll.anchor_y
            + near_bottom.autoscroll.dead_zone_radius
            > near_bottom.viewport.bottom,
        "near-bottom graphic must rely on the existing viewport clip");
    input.autoscroll_anchor_y = input.content_viewport.bottom;
    expect(!mv::build_comic_controls_layout(input).autoscroll.visible,
        "anchor center on the half-open content bottom edge must be outside");

    const float usable_right = forward.scrollbar.hit_bounds.left;
    input.autoscroll_anchor_x = usable_right - 1.0f;
    input.autoscroll_anchor_y = 400.0f;
    input.autoscroll_pointer_y = input.autoscroll_anchor_y + 240.0f;
    const mv::ComicControlsLayout near_right =
        mv::build_comic_controls_layout(input);
    expect(near_right.autoscroll.visible
            && near_right.autoscroll.direction
                == mv::ComicAutoscrollDirection::Forward
            && std::isfinite(near_right.autoscroll.arrow_tail_y)
            && std::isfinite(near_right.autoscroll.arrow_tip_y),
        "anchor center near usable right edge must remain visible and finite");

    input.autoscroll_anchor_x = usable_right;
    expect(!mv::build_comic_controls_layout(input).autoscroll.visible,
        "anchor center in scrollbar hit zone must retain precedence and fail closed");

    input.virtual_height = input.content_viewport.height();
    input.autoscroll_anchor_x = input.content_viewport.right - 1.0f;
    expect(mv::build_comic_controls_layout(input).autoscroll.visible,
        "adjacent pixel inside a content edge must remain a valid anchor");
    input.autoscroll_anchor_x = input.content_viewport.right;
    expect(!mv::build_comic_controls_layout(input).autoscroll.visible,
        "anchor center on panel-left/content-right boundary must be outside");
}

void test_comic_controls_invalid_input_fails_closed() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const float maximum = std::numeric_limits<float>::max();
    mv::ComicControlsRenderInput input;
    input.content_viewport = {0.0f, 40.0f, 1000.0f, 840.0f};
    input.dpi = 96.0f;
    input.virtual_height = 4000.0f;
    input.scroll_y = 800.0f;
    input.anchored_page_index = 11;
    input.total_pages = 86;
    input.page_badge_text = L"12 / 86";
    input.transient_text = L"page.png \u00B7 \u7B2C12/86\u9875";
    input.transient_kind = mv::ComicTransientOverlayKind::PageChange;

    input.anchored_page_index = -1;
    mv::ComicControlsLayout invalid_page =
        mv::build_comic_controls_layout(input);
    expect(!invalid_page.page_badge.visible
            && !invalid_page.transient_overlay.visible,
        "negative page anchor must suppress progress and page toast");
    input.anchored_page_index = 86;
    invalid_page = mv::build_comic_controls_layout(input);
    expect(!invalid_page.page_badge.visible
            && !invalid_page.transient_overlay.visible,
        "anchor at total page count must fail closed");
    input.anchored_page_index = 0;
    input.total_pages = 0;
    invalid_page = mv::build_comic_controls_layout(input);
    expect(!invalid_page.page_badge.visible
            && !invalid_page.transient_overlay.visible,
        "non-positive total pages must suppress page feedback");
    input.transient_kind = mv::ComicTransientOverlayKind::Status;
    input.transient_text = L"\u81EA\u52A8\u6EDA\u52A8\u5DF2\u6682\u505C";
    const mv::ComicControlsLayout status_without_page =
        mv::build_comic_controls_layout(input);
    expect(!status_without_page.page_badge.visible
            && status_without_page.transient_overlay.visible,
        "status feedback must remain independent from page validity");

    input.total_pages = 86;
    input.transient_kind = mv::ComicTransientOverlayKind::PageChange;
    input.scroll_y = nan;
    const mv::ComicControlsLayout nan_scroll =
        mv::build_comic_controls_layout(input);
    expect(!nan_scroll.scrollbar.visible,
        "NaN scroll offset must suppress scrollbar geometry");
    input.scroll_y = 800.0f;
    input.virtual_height = infinity;
    expect(!mv::build_comic_controls_layout(input).scrollbar.visible,
        "infinite virtual canvas height must suppress scrollbar geometry");
    input.virtual_height = 4000.0f;
    input.dpi = maximum;
    const mv::ComicControlsLayout overflow =
        mv::build_comic_controls_layout(input);
    expect(overflow.viewport.empty() && !overflow.page_badge.visible,
        "DPI-derived metric overflow must fail closed before D2D");
    input.dpi = 96.0f;
    input.content_viewport = {-maximum, 0.0f, maximum, 100.0f};
    const mv::ComicControlsLayout viewport_overflow =
        mv::build_comic_controls_layout(input);
    expect(viewport_overflow.viewport.empty()
            && !viewport_overflow.scrollbar.visible,
        "viewport derived-width overflow must fail closed before layout");

    const mv::ComicScrollbarGeometry geometry =
        mv::build_comic_scrollbar_geometry(
            {0.0f, 40.0f, 1000.0f, 840.0f},
            4000.0f, 800.0f, 96.0f);
    expect(mv::hit_test_comic_scrollbar(geometry, nan, 100.0f)
            == mv::ComicScrollbarHit::None,
        "NaN pointer must not hit scrollbar geometry");
    expect(!mv::map_comic_scrollbar_drag(
            geometry, infinity, 1.0f).valid,
        "infinite drag pointer must not map to virtual scroll");
    expect(!mv::map_comic_scrollbar_drag(
            geometry, geometry.thumb.top, -1.0f).valid,
        "grab offset outside thumb must fail closed");
}

} // namespace

int main() {
    expect(!mv::should_recreate_render_device(S_OK),
        "successful rendering must keep the current device");
    expect(mv::should_recreate_render_device(D2DERR_RECREATE_TARGET),
        "D2D target loss must recreate the renderer");
    expect(mv::should_recreate_render_device(DXGI_ERROR_DEVICE_REMOVED),
        "DXGI device removal must recreate the renderer");
    expect(mv::should_recreate_render_device(DXGI_ERROR_DEVICE_RESET),
        "DXGI device reset must recreate the renderer");
    expect(!mv::should_recreate_render_device(E_FAIL),
        "unclassified Present/Resize business failures must keep the current device");

    expect(mv::renderer_generation_changed(1, 2),
        "App caches must be invalidated after renderer recreation");
    expect(!mv::renderer_generation_changed(2, 2),
        "App caches should remain valid within one renderer generation");

    // Deterministic metric fixtures represent DWrite results for short Chinese,
    // long Chinese, and English/path feedback without depending on installed fonts.
    auto expect_scaled_short_chinese = [](float dpi) {
        const float scale = dpi / 96.0f;
        const auto layout = mv::calculate_panel_toast_layout({
            20.0f, 30.0f, 280.0f * scale, 400.0f * scale,
            44.0f * scale, 15.0f * scale, 1, dpi});
        expect(layout.single_line,
            "short Chinese feedback must remain on one line");
        expect(nearly_equal(layout.bounds.right - layout.bounds.left,
                96.0f * scale),
            "short Chinese feedback must use the DPI-scaled minimum width");
        expect(nearly_equal(layout.text_bounds.left - layout.bounds.left,
                10.0f * scale)
                && nearly_equal(layout.text_bounds.top - layout.bounds.top,
                    7.0f * scale),
            "toast text padding must scale with DPI");
        expect(nearly_equal(layout.corner_radius, 4.0f * scale),
            "toast corner radius must scale with DPI");
    };
    expect_scaled_short_chinese(96.0f);
    expect_scaled_short_chinese(168.0f);
    expect_scaled_short_chinese(192.0f);

    const auto long_chinese = mv::calculate_panel_toast_layout({
        10.0f, 20.0f, 490.0f, 700.0f,
        600.0f, 84.0f, 3, 168.0f});
    expect(!long_chinese.single_line,
        "long Chinese feedback must retain its measured multi-line layout");
    expect(nearly_equal(long_chinese.bounds.right - long_chinese.bounds.left,
            406.0f),
        "long Chinese feedback must stop at the panel-relative maximum width");
    expect(nearly_equal(long_chinese.bounds.bottom, 678.0f)
            && nearly_equal(long_chinese.bounds.top, 569.5f),
        "multi-line feedback must grow upward from the DPI-scaled bottom inset");

    const auto english_path = mv::calculate_panel_toast_layout({
        0.0f, 0.0f, 560.0f, 500.0f,
        250.0f, 30.0f, 1, 192.0f});
    expect(english_path.single_line
            && nearly_equal(english_path.bounds.right
                - english_path.bounds.left, 294.0f),
        "an English path that fits must keep a measured single-line width");

    const auto narrow_path = mv::calculate_panel_toast_layout({
        50.0f, 60.0f, 240.0f, 160.0f,
        800.0f, 80.0f, 4, 192.0f});
    expect(!narrow_path.single_line
            && nearly_equal(narrow_path.bounds.left, 98.0f)
            && nearly_equal(narrow_path.bounds.right, 242.0f),
        "a long path in a narrow panel must use the clamped maximum width");
    expect(nearly_equal(narrow_path.bounds.top, 108.0f)
            && nearly_equal(narrow_path.bounds.bottom, 172.0f)
            && nearly_equal(narrow_path.text_bounds.bottom
                - narrow_path.text_bounds.top, 36.0f),
        "a narrow panel must clamp multi-line height inside its visible area");
    expect(narrow_path.bounds.left >= 50.0f
            && narrow_path.bounds.right <= 290.0f
            && narrow_path.bounds.top >= 60.0f
            && narrow_path.bounds.bottom <= 220.0f,
        "toast geometry must remain inside an offset panel viewport");
    test_dpi_metrics();
    test_transition_ease();
    test_transition_ease_exit();
    test_transition_veil_exit();
    test_device_loss_detection();
    test_narrow_viewport_and_error_card();
    test_wide_viewport_and_seamless_gap();
    test_clip_boundaries();
    test_non_finite_geometry_fails_closed();
    test_comic_scrollbar_dpi_and_boundaries();
    test_comic_scrollbar_hit_test_and_drag();
    test_comic_scrollbar_half_open_boundaries();
    test_comic_controls_viewport_avoidance();
    test_comic_controls_transient_layout();
    test_comic_autoscroll_graphic();
    test_comic_controls_invalid_input_fails_closed();

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }

    std::cout << "renderer_state_tests: PASS\n";
    return 0;
}
