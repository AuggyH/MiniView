#include "renderer_state.h"

#include <d2d1.h>
#include <dxgi.h>
#include <cmath>
#include <iostream>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void expect_near(float actual, float expected, const char* message) {
    if (std::fabs(actual - expected) > 0.01f) {
        std::cerr << "FAIL: " << message << " (expected " << expected
                  << ", got " << actual << ")\n";
        ++failures;
    }
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
    expect(mv::should_recreate_render_device(E_FAIL),
        "unclassified failed Present or Resize results must fail closed");

    expect(mv::renderer_generation_changed(1, 2),
        "App caches must be invalidated after renderer recreation");
    expect(!mv::renderer_generation_changed(2, 2),
        "App caches should remain valid within one renderer generation");

    test_dpi_metrics();
    test_narrow_viewport_and_error_card();
    test_wide_viewport_and_seamless_gap();
    test_clip_boundaries();

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }

    std::cout << "renderer_state_tests: PASS\n";
    return 0;
}
