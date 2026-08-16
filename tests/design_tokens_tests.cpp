// Unit tests for Phase 1 design tokens (colors only).
// Run via CTest (design_tokens.unit).

#include "design_tokens.h"

#include <cmath>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <stdexcept>

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
