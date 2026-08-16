// Unit tests for Phase 1+2 design tokens (colors, spacing, typography).
// Run via CTest (design_tokens.unit).

#include "design_tokens.h"
#include "layout.h"

#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string_view>

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

void test_d2d_round_trip() {
    const std::initializer_list<mv::dt::Color> colors = {
        mv::dt::kColorCanvas,
        mv::dt::kColorPlaceholder,
        mv::dt::kColorOverlayText,
        mv::dt::kColorHintText,
        mv::dt::kColorStatusErrorBg,
        mv::dt::kColorStatusErrorBorder,
        mv::dt::kColorStatusErrorText,
        mv::dt::kColorInfoCardBg,
        mv::dt::kColorInfoCardBorder,
        mv::dt::kColorInfoLabel,
        mv::dt::kColorInfoValue,
        mv::dt::kColorInfoDim,
        mv::dt::kColorOverlayShadow,
        mv::dt::kColorPlaceholderMin,
        mv::dt::kColorFavouriteChip,
        mv::dt::kColorFavouriteHeart,
        mv::dt::kColorComicErrorBg,
        mv::dt::kColorComicPlaceholderBg,
        mv::dt::kColorComicErrorBorder,
        mv::dt::kColorComicPlaceholderBorder,
        mv::dt::kColorComicErrorText,
        mv::dt::kColorComicScrollbarTrack,
        mv::dt::kColorComicScrollbarThumbActive,
        mv::dt::kColorComicScrollbarThumbHover,
        mv::dt::kColorComicScrollbarThumbIdle,
        mv::dt::kColorComicOverlayBg,
        mv::dt::kColorComicOverlayBorder,
        mv::dt::kColorComicOverlayText,
        mv::dt::kColorAutoscrollZone,
        mv::dt::kColorAutoscrollRing,
        mv::dt::kColorAutoscrollArrow,
        mv::dt::kColorWidthSliderTrack,
        mv::dt::kColorWidthSliderThumb,
        mv::dt::kColorTextDim,
        mv::dt::kColorCruiseIndicator,
        mv::dt::kColorSelectionAccent,
        mv::dt::kColorNavAccent,
        mv::dt::kColorLabelDefault,
        mv::dt::kColorPanelBg,
        mv::dt::kColorPanelDivider,
        mv::dt::kColorPanelLabel,
        mv::dt::kColorPanelValue,
        mv::dt::kColorPanelSelection,
        mv::dt::kColorPanelSectionTitle,
        mv::dt::kColorPanelToastBg,
        mv::dt::kColorPanelToastText,
        mv::dt::kColorScrollbarThumbActive,
        mv::dt::kColorScrollbarThumbIdle,
        mv::dt::kColorBlack,
        mv::dt::kColorFilmstripArrow,
        mv::dt::kColorToolbarBg,
        mv::dt::kColorToolbarText,
        mv::dt::kColorToolbarHoverBg,
        mv::dt::kColorTitleText,
        mv::dt::kColorTitleMenuText,
        mv::dt::kColorTitleMenuHoverBg,
        mv::dt::kColorWindowButtonCloseHover,
        mv::dt::kColorWindowButtonHover,
        mv::dt::kColorWindowButtonSymbol,
        mv::dt::kColorBreadcrumbHover,
        mv::dt::kColorBreadcrumbLine,
        mv::dt::kColorNavLine,
        mv::dt::kColorNavDim,
        mv::dt::kColorNavBright,
        mv::dt::kColorNavHoverBg,
        mv::dt::kColorNavBadge,
        mv::dt::kColorNavError,
    };

    for (const mv::dt::Color color : colors) {
        const D2D1_COLOR_F converted = mv::dt::d2d(color);
        expect_near(converted.r, color.r, 0.0f, "d2d round-trip red");
        expect_near(converted.g, color.g, 0.0f, "d2d round-trip green");
        expect_near(converted.b, color.b, 0.0f, "d2d round-trip blue");
        expect_near(converted.a, color.a, 0.0f, "d2d round-trip alpha");
    }
}

void test_with_alpha() {
    const std::initializer_list<mv::dt::Color> colors = {
        mv::dt::kColorCanvas,
        mv::dt::kColorPlaceholder,
        mv::dt::kColorSelectionAccent,
        mv::dt::kColorNavAccent,
        mv::dt::kColorStatusErrorBg,
        mv::dt::kColorBlack,
    };

    constexpr float kAlpha = 0.42f;
    for (const mv::dt::Color color : colors) {
        const mv::dt::Color adjusted = mv::dt::with_alpha(color, kAlpha);
        expect_near(adjusted.r, color.r, 0.0f, "with_alpha preserves red");
        expect_near(adjusted.g, color.g, 0.0f, "with_alpha preserves green");
        expect_near(adjusted.b, color.b, 0.0f, "with_alpha preserves blue");
        expect_near(adjusted.a, kAlpha, 0.0f, "with_alpha sets alpha");
    }
}

void test_drifted_accent_blues_remain_distinct() {
    // The audit's drifted accent blues must stay two separate tokens.
    expect_near(mv::dt::kColorSelectionAccent.b, 1.0f, 0.0f,
        "selection accent blue stays 1.0");
    expect_near(mv::dt::kColorNavAccent.b, 0.89f, 0.0f,
        "nav accent blue stays 0.89");
}

void test_dip() {
    expect_near(mv::dt::scale(96.0f), 1.0f, 0.0f, "scale 96 -> 1x");
    expect_near(mv::dt::scale(192.0f), 2.0f, 0.0f, "scale 192 -> 2x");
    expect_near(mv::dt::scale(0.0f), 1.0f, 0.0f, "scale 0 -> 1x");
    expect_near(mv::dt::scale(-96.0f), 1.0f, 0.0f, "scale negative -> 1x");

    expect_near(mv::dt::dip(8.0f, 96.0f), 8.0f, 0.0f, "dip 8 @96 -> 8");
    expect_near(mv::dt::dip(8.0f, 192.0f), 16.0f, 0.0f, "dip 8 @192 -> 16");
    expect_near(mv::dt::dip(8.0f, 0.0f), 8.0f, 0.0f, "dip 8 @0 -> 8 (1x)");
    expect_near(mv::dt::dip(8.0f, -96.0f), 8.0f, 0.0f,
        "dip 8 @negative -> 8 (1x)");
}

void test_spacing_tokens() {
    expect_near(mv::dt::kSpace3Dip, 3.0f, 0.0f, "kSpace3Dip == 3");
    expect_near(mv::dt::kSpaceXsDip, 4.0f, 0.0f, "kSpaceXsDip == 4");
    expect_near(mv::dt::kSpace6Dip, 6.0f, 0.0f, "kSpace6Dip == 6");
    expect_near(mv::dt::kSpace7Dip, 7.0f, 0.0f, "kSpace7Dip == 7");
    expect_near(mv::dt::kSpaceSmDip, 8.0f, 0.0f, "kSpaceSmDip == 8");
    expect_near(mv::dt::kSpace10Dip, 10.0f, 0.0f, "kSpace10Dip == 10");
    expect_near(mv::dt::kSpaceMdDip, 12.0f, 0.0f, "kSpaceMdDip == 12");
    expect_near(mv::dt::kSpace14Dip, 14.0f, 0.0f, "kSpace14Dip == 14");
    expect_near(mv::dt::kSpaceLgDip, 16.0f, 0.0f, "kSpaceLgDip == 16");
    expect_near(mv::dt::kSpace20Dip, 20.0f, 0.0f, "kSpace20Dip == 20");
    expect_near(mv::dt::kSpaceXlDip, 24.0f, 0.0f, "kSpaceXlDip == 24");
    expect_near(mv::dt::kSize28Dip, 28.0f, 0.0f, "kSize28Dip == 28");
    expect_near(mv::dt::kSpace2xlDip, 32.0f, 0.0f, "kSpace2xlDip == 32");
    expect_near(mv::dt::kSize36Dip, 36.0f, 0.0f, "kSize36Dip == 36");
    expect_near(mv::dt::kSpace40Dip, 40.0f, 0.0f, "kSpace40Dip == 40");
    expect_near(mv::dt::kSize44Dip, 44.0f, 0.0f, "kSize44Dip == 44");
    expect_near(mv::dt::kSize64Dip, 64.0f, 0.0f, "kSize64Dip == 64");
    expect_near(mv::dt::kSize96Dip, 96.0f, 0.0f, "kSize96Dip == 96");
    expect_near(mv::dt::kSize240Dip, 240.0f, 0.0f, "kSize240Dip == 240");
    expect_near(mv::dt::kSize480Dip, 480.0f, 0.0f, "kSize480Dip == 480");
}

void test_typography_tokens() {
    expect_near(mv::dt::kFontSizeXsDip, 10.0f, 0.0f, "kFontSizeXsDip == 10");
    expect_near(mv::dt::kFontSizeSmDip, 11.0f, 0.0f, "kFontSizeSmDip == 11");
    expect_near(mv::dt::kFontSizeMdDip, 12.0f, 0.0f, "kFontSizeMdDip == 12");
    expect_near(mv::dt::kFontSizeLgDip, 13.0f, 0.0f, "kFontSizeLgDip == 13");
    expect_near(mv::dt::kFontSizeXlDip, 14.0f, 0.0f, "kFontSizeXlDip == 14");

    expect(std::wstring_view(mv::dt::kFontFamilyUi) == L"Microsoft YaHei",
        "kFontFamilyUi stays Microsoft YaHei");
    expect(std::wstring_view(mv::dt::kFontFamilySymbols) == L"Segoe UI",
        "kFontFamilySymbols stays Segoe UI");

    expect(mv::dt::kFontWeightNormal == DWRITE_FONT_WEIGHT_NORMAL,
        "kFontWeightNormal == DWRITE_FONT_WEIGHT_NORMAL");
    expect(mv::dt::kFontWeightBold == DWRITE_FONT_WEIGHT_BOLD,
        "kFontWeightBold == DWRITE_FONT_WEIGHT_BOLD");
}

void test_motion_tokens() {
    expect_near(mv::dt::kDurationTransitionSec, 0.25f, 0.0f,
        "kDurationTransitionSec == 0.25 (录屏拟合 249/251ms)");
    expect_near(mv::dt::kDurationFilmstripHandoffSec, 0.30f, 0.0f,
        "kDurationFilmstripHandoffSec == 0.30 (filmstrip keeps 300ms)");
    expect_near(mv::dt::kDurationFilmstripHideSec, 3.0f, 0.0f,
        "kDurationFilmstripHideSec == 3.0 (静止 3s 收起)");
    expect_near(mv::dt::kDurationFilmstripRevealSec, 0.25f, 0.0f,
        "kDurationFilmstripRevealSec == 0.25 (升起/收起动画)");
    expect(mv::dt::kDurationToastMs == 1000, "kDurationToastMs == 1000");
    expect(mv::dt::kDurationSelectionHighlightMs == 1000,
        "kDurationSelectionHighlightMs == 1000");
    expect(mv::dt::kDurationImageDebounceMs == 250,
        "kDurationImageDebounceMs == 250");
    expect(mv::dt::kDurationScrollPauseMs == 80, "kDurationScrollPauseMs == 80");
    expect(mv::dt::kDurationRenderRetryMs == 120,
        "kDurationRenderRetryMs == 120");
    expect(mv::dt::kAnimationFrameMs == 16, "kAnimationFrameMs == 16");
}

void test_layout_aliases() {
    using namespace mv;

    // layout.h constants must be value-identical to their design tokens.
    expect(layout::kBaseDpi == dt::kBaseDpi, "kBaseDpi alias == dt::kBaseDpi");

    expect(static_cast<float>(layout::kTitleBarHeightDip) == dt::kSpace40Dip,
        "kTitleBarHeightDip alias == dt::kSpace40Dip");
    expect(layout::kTitleBarPadDip == dt::kSpaceMdDip,
        "kTitleBarPadDip alias == dt::kSpaceMdDip");
    expect(layout::kTitleBarTitleWidthDip == dt::kTitleBarTitleWidthDip,
        "kTitleBarTitleWidthDip alias == dt::kTitleBarTitleWidthDip");
    expect(layout::kTitleBarTitleGapDip == dt::kSpaceXsDip,
        "kTitleBarTitleGapDip alias == dt::kSpaceXsDip");
    expect(layout::kTitleBarMenuFontSizeDip == dt::kFontSizeMdDip,
        "kTitleBarMenuFontSizeDip alias == dt::kFontSizeMdDip");
    expect(layout::kTitleBarMenuPadDip == dt::kSpaceLgDip,
        "kTitleBarMenuPadDip alias == dt::kSpaceLgDip");
    expect(layout::kTitleBarButtonWidthDip == dt::kTitleBarButtonWidthDip,
        "kTitleBarButtonWidthDip alias == dt::kTitleBarButtonWidthDip");

    expect(layout::kThumbCellDip == dt::kThumbCellDip,
        "kThumbCellDip alias == dt::kThumbCellDip");
    expect(layout::kThumbSizeDip == dt::kThumbSizeDip,
        "kThumbSizeDip alias == dt::kThumbSizeDip");
    expect(static_cast<float>(layout::kThumbGapHDip) == dt::kSpaceSmDip,
        "kThumbGapHDip alias == dt::kSpaceSmDip");
    expect(static_cast<float>(layout::kThumbGapVDip) == dt::kSpaceLgDip,
        "kThumbGapVDip alias == dt::kSpaceLgDip");
    expect(static_cast<float>(layout::kThumbPadDip) == dt::kSpaceSmDip,
        "kThumbPadDip alias == dt::kSpaceSmDip");
    expect(layout::kGridLabelHeightDip == dt::kGridLabelHeightDip,
        "kGridLabelHeightDip alias == dt::kGridLabelHeightDip");
    expect(layout::kThumbCornerRadiusDip == dt::kSpaceXsDip,
        "kThumbCornerRadiusDip alias == dt::kSpaceXsDip");

    expect(layout::kPanelWidthDip == dt::kPanelWidthDip,
        "kPanelWidthDip alias == dt::kPanelWidthDip");
    expect(layout::kPanelLabelColumnWidthDip == dt::kPanelLabelColumnWidthDip,
        "kPanelLabelColumnWidthDip alias == dt::kPanelLabelColumnWidthDip");

    expect(static_cast<float>(layout::kNavPanelWidthDip) == dt::kSize240Dip,
        "kNavPanelWidthDip alias == dt::kSize240Dip");
    expect(layout::kNavBreadcrumbBarHeightDip == dt::kSize36Dip,
        "kNavBreadcrumbBarHeightDip alias == dt::kSize36Dip");
    expect(layout::kNavTabHeightDip == dt::kSize28Dip,
        "kNavTabHeightDip alias == dt::kSize28Dip");
    expect(layout::kNavRowHeightDip == dt::kSize28Dip,
        "kNavRowHeightDip alias == dt::kSize28Dip");
    expect(layout::kNavIndentDip == dt::kSpaceLgDip,
        "kNavIndentDip alias == dt::kSpaceLgDip");
    expect(layout::kNavArrowWidthDip == dt::kSpace20Dip,
        "kNavArrowWidthDip alias == dt::kSpace20Dip");
    expect(layout::kNavStatsHeightDip == dt::kSpaceXlDip,
        "kNavStatsHeightDip alias == dt::kSpaceXlDip");
    expect(layout::kNavScrollbarWidthDip == dt::kSpace6Dip,
        "kNavScrollbarWidthDip alias == dt::kSpace6Dip");
    expect(layout::kNavToggleButtonWidthDip == dt::kSize44Dip,
        "kNavToggleButtonWidthDip alias == dt::kSize44Dip");
    expect(layout::kNavFontSizeDip == dt::kFontSizeMdDip,
        "kNavFontSizeDip alias == dt::kFontSizeMdDip");
    expect(layout::kNavSmallFontSizeDip == dt::kFontSizeXsDip,
        "kNavSmallFontSizeDip alias == dt::kFontSizeXsDip");
    expect(layout::kNavPadDip == dt::kSpaceSmDip,
        "kNavPadDip alias == dt::kSpaceSmDip");
    expect(layout::kNavBreadcrumbGapDip == dt::kSpaceXsDip,
        "kNavBreadcrumbGapDip alias == dt::kSpaceXsDip");

    expect(static_cast<float>(layout::kScrollbarZoneDip) == dt::kSpace20Dip,
        "kScrollbarZoneDip alias == dt::kSpace20Dip");

    expect(layout::kFilmstripHeightDip == dt::kFilmstripHeightDip,
        "kFilmstripHeightDip alias == dt::kFilmstripHeightDip");
    expect(layout::kFilmstripPadVDip == dt::kSpaceSmDip,
        "kFilmstripPadVDip alias == dt::kSpaceSmDip");
    expect(layout::kFilmstripThumbHeightDip == dt::kSize64Dip,
        "kFilmstripThumbHeightDip alias == dt::kSize64Dip");
    expect(layout::kFilmstripGapDip == dt::kSpaceSmDip,
        "kFilmstripGapDip alias == dt::kSpaceSmDip");
    expect(layout::kFilmstripCurrentScale == dt::kFilmstripCurrentScale,
        "kFilmstripCurrentScale alias == dt::kFilmstripCurrentScale");
    expect(layout::kFilmstripBorderDip == dt::kFilmstripBorderDip,
        "kFilmstripBorderDip alias == dt::kFilmstripBorderDip");
    expect(layout::kFilmstripIndicatorDip == dt::kSpaceXsDip,
        "kFilmstripIndicatorDip alias == dt::kSpaceXsDip");
    expect(layout::kFilmstripArrowZoneDip == dt::kSpaceXlDip,
        "kFilmstripArrowZoneDip alias == dt::kSpaceXlDip");
    expect(layout::kFilmstripHoverZoneDip == dt::kSpaceXlDip,
        "kFilmstripHoverZoneDip alias == dt::kSpaceXlDip");
    expect(layout::kFilmstripHideDelaySeconds == dt::kDurationFilmstripHideSec,
        "kFilmstripHideDelaySeconds alias == dt::kDurationFilmstripHideSec");
    expect(layout::kFilmstripMaxAspect == dt::kFilmstripMaxAspect,
        "kFilmstripMaxAspect alias == dt::kFilmstripMaxAspect");

    expect(layout::kDefaultWindowWidthDip == dt::kDefaultWindowWidthDip,
        "kDefaultWindowWidthDip alias == dt::kDefaultWindowWidthDip");
    expect(layout::kDefaultWindowHeightDip == dt::kDefaultWindowHeightDip,
        "kDefaultWindowHeightDip alias == dt::kDefaultWindowHeightDip");

    // layout::dpi_scale keeps its original (no <=0 guard) semantics.
    expect_near(layout::dpi_scale(96.0f), 1.0f, 0.0f,
        "layout::dpi_scale(96) == 1");
    expect_near(layout::dpi_scale(192.0f), 2.0f, 0.0f,
        "layout::dpi_scale(192) == 2");
    expect_near(layout::dpi_scale(0.0f), 0.0f, 0.0f,
        "layout::dpi_scale(0) == 0");
}

} // namespace

int main() {
    struct TestCase {
        const char* name;
        void (*fn)();
    } cases[] = {
        {"d2d_round_trip", test_d2d_round_trip},
        {"with_alpha", test_with_alpha},
        {"drifted_accent_blues_remain_distinct",
            test_drifted_accent_blues_remain_distinct},
        {"dip", test_dip},
        {"spacing_tokens", test_spacing_tokens},
        {"typography_tokens", test_typography_tokens},
        {"motion_tokens", test_motion_tokens},
        {"layout_aliases", test_layout_aliases},
    };

    int failures = 0;
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
        std::cout << "design_tokens: all " << sizeof(cases) / sizeof(cases[0])
                  << " tests passed\n";
        return 0;
    }
    std::cerr << "design_tokens: " << failures << " test(s) failed\n";
    return 1;
}
