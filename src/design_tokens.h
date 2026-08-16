#pragma once
// Phase 1 design tokens: colors only (no spacing/font/radius/motion yet).
// Header-only, zero external dependencies, C++20 inline constexpr.
// D2D call sites use dt::d2d(dt::kColorX); GDI call sites use dt::kColorXGdi.

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <string_view>

namespace mv::dt {

struct Color {
    float r;
    float g;
    float b;
    float a;
};

// Converts a token color to a D2D1_COLOR_F (aggregate; constexpr, zero cost).
constexpr D2D1_COLOR_F d2d(Color c) noexcept {
    return D2D1_COLOR_F{c.r, c.g, c.b, c.a};
}

// Preserves RGB and replaces alpha.
constexpr Color with_alpha(Color c, float alpha) noexcept {
    return Color{c.r, c.g, c.b, alpha};
}

// ── DPI scaling helpers ──────────────────────────────────────
// All DIP tokens are nominal values at 96 DPI. Pass a real DPI (e.g.
// GetDpiForWindow or renderer dpi state) to dip(); invalid dpi (<= 0)
// falls back to 1.0x so callers never divide by zero.
inline constexpr float kBaseDpi = 96.0f;
static_assert(kBaseDpi == 96.0f, "design token value changed: kBaseDpi");

inline constexpr float scale(float dpi) noexcept {
    return dpi > 0.0f ? dpi / kBaseDpi : 1.0f;
}

inline constexpr float dip(float valueDip, float dpi) noexcept {
    return valueDip * scale(dpi);
}

// ── Spacing / size DIP tokens ────────────────────────────────
// T-shirt names cover the canonical 4/8/12/16/24/32 spacing scale from
// the audit. Numeric kSpace… / kSize… names cover the remaining recurring
// DIP values migrated from panel/card/status/toast/nav/metrics code.
#define MV_DT_DIP(name, value)                                      \
    inline constexpr float name = (value);                          \
    static_assert(name == (value), "design token value changed: " #name)

MV_DT_DIP(kSpace3Dip,   3.0f);
MV_DT_DIP(kSpaceXsDip,  4.0f);
MV_DT_DIP(kSpace6Dip,   6.0f);
MV_DT_DIP(kSpace7Dip,   7.0f);
MV_DT_DIP(kSpaceSmDip,  8.0f);
MV_DT_DIP(kSpace10Dip, 10.0f);
MV_DT_DIP(kSpaceMdDip, 12.0f);
MV_DT_DIP(kSpace14Dip, 14.0f);
MV_DT_DIP(kSpaceLgDip, 16.0f);
MV_DT_DIP(kSpace20Dip, 20.0f);
MV_DT_DIP(kSpaceXlDip, 24.0f);
MV_DT_DIP(kSize28Dip,  28.0f);
MV_DT_DIP(kSpace2xlDip, 32.0f);
MV_DT_DIP(kSize36Dip,  36.0f);
MV_DT_DIP(kSpace40Dip, 40.0f);
MV_DT_DIP(kSize44Dip,  44.0f);
MV_DT_DIP(kSize64Dip,  64.0f);
MV_DT_DIP(kSize96Dip,  96.0f);
MV_DT_DIP(kSize240Dip, 240.0f);
MV_DT_DIP(kSize480Dip, 480.0f);

// ── Component visual constants (Stage 2 renderer hardening) ──
// Values are DIP unless the name says otherwise; all migrated from
// renderer.cpp magic numbers without changing the pixel values.
MV_DT_DIP(kStatusMessageMaxWidthDip,    520.0f);
MV_DT_DIP(kStatusMessageCornerRadiusDip,  6.0f);
MV_DT_DIP(kInfoCardMaxWidthDip,         600.0f);
MV_DT_DIP(kInfoCardCornerRadiusDip,       8.0f);
MV_DT_DIP(kTitleBarMenuHoverRadiusDip,    4.0f);
MV_DT_DIP(kTitleBarMenuHoverInsetYDip,    2.0f);
MV_DT_DIP(kSelectionBorderWidthDip,       2.0f);
MV_DT_DIP(kPanelPreviewCornerRadiusDip,   4.0f);
MV_DT_DIP(kPanelSelectionCornerRadiusDip, 2.0f);
MV_DT_DIP(kComicOverlayCornerRadiusMaxDip, 8.0f);

#undef MV_DT_DIP

// Physical-pixel component constants from renderer.cpp. These were already
// used as raw physical px by the renderer (not DPI-scaled), so they keep
// their exact values and the _Px suffix to prevent accidental DPI scaling.
#define MV_DT_PX(name, value)                                       \
    inline constexpr float name = (value);                          \
    static_assert(name == (value), "design token value changed: " #name)

MV_DT_PX(kToolbarPadXPx,               12.0f);
MV_DT_PX(kToolbarItemInsetYPx,          2.0f);
MV_DT_PX(kScrollbarMinThumbPx,         28.0f);
MV_DT_PX(kScrollbarActiveInsetPx,       2.0f);
MV_DT_PX(kScrollbarIdleInsetPx,         6.0f);
MV_DT_PX(kComicWidthSliderTrackRadiusPx, 2.0f);

#undef MV_DT_PX

// ── Phase 4: layout geometry tokens (merged from layout.h) ──
// Values that already exist as Phase 2 kSpace/kSize/font tokens are reused
// directly by layout.h (e.g. 40 -> kSpace40Dip, 240 -> kSize240Dip). Only
// geometry values with no Phase 2 token are added here, with the same type
// as their layout.h originals so layout.h can alias them with `auto`.
#define MV_DT_LAYOUT_TOKEN(name, value)                             \
    inline constexpr auto name = (value);                           \
    static_assert(name == (value), "design token value changed: " #name)

MV_DT_LAYOUT_TOKEN(kTitleBarTitleWidthDip, 68.0f);
MV_DT_LAYOUT_TOKEN(kTitleBarButtonWidthDip, 46.0f);
MV_DT_LAYOUT_TOKEN(kThumbCellDip, 200);
MV_DT_LAYOUT_TOKEN(kThumbSizeDip, 400);
MV_DT_LAYOUT_TOKEN(kGridLabelHeightDip, 42);
MV_DT_LAYOUT_TOKEN(kPanelWidthDip, 280);
MV_DT_LAYOUT_TOKEN(kPanelLabelColumnWidthDip, 70.0f);
MV_DT_LAYOUT_TOKEN(kFilmstripHeightDip, 80.0f);
MV_DT_LAYOUT_TOKEN(kFilmstripCurrentScale, 1.25f);
MV_DT_LAYOUT_TOKEN(kFilmstripBorderDip, 2.0f);
MV_DT_LAYOUT_TOKEN(kFilmstripMaxAspect, 10.0f);
MV_DT_LAYOUT_TOKEN(kDefaultWindowWidthDip, 1400);
MV_DT_LAYOUT_TOKEN(kDefaultWindowHeightDip, 900);

#undef MV_DT_LAYOUT_TOKEN

// ── Typography tokens ─────────────────────────────────────────
// Font sizes are logical pt; DWrite call sites DPI-scale them.
#define MV_DT_FONT_SIZE(name, value)                                \
    inline constexpr float name = (value);                          \
    static_assert(name == (value), "design token value changed: " #name)

MV_DT_FONT_SIZE(kFontSizeXsDip, 10.0f);
MV_DT_FONT_SIZE(kFontSizeSmDip, 11.0f);
MV_DT_FONT_SIZE(kFontSizeMdDip, 12.0f);
MV_DT_FONT_SIZE(kFontSizeLgDip, 13.0f);
MV_DT_FONT_SIZE(kFontSizeXlDip, 14.0f);

#undef MV_DT_FONT_SIZE

inline constexpr wchar_t kFontFamilyUi[] = L"Microsoft YaHei";
inline constexpr wchar_t kFontFamilySymbols[] = L"Segoe UI";
static_assert(std::wstring_view(kFontFamilyUi) == L"Microsoft YaHei",
    "design token value changed: kFontFamilyUi");
static_assert(std::wstring_view(kFontFamilySymbols) == L"Segoe UI",
    "design token value changed: kFontFamilySymbols");

inline constexpr DWRITE_FONT_WEIGHT kFontWeightNormal =
    DWRITE_FONT_WEIGHT_NORMAL;
inline constexpr DWRITE_FONT_WEIGHT kFontWeightBold =
    DWRITE_FONT_WEIGHT_BOLD;
static_assert(kFontWeightNormal == DWRITE_FONT_WEIGHT_NORMAL,
    "design token value changed: kFontWeightNormal");
static_assert(kFontWeightBold == DWRITE_FONT_WEIGHT_BOLD,
    "design token value changed: kFontWeightBold");

// ── D2D colors ───────────────────────────────────────────────
#define MV_DT_COLOR(name, red, green, blue, alpha)                 \
    inline constexpr Color name{red, green, blue, alpha};          \
    static_assert(name.r == (red) && name.g == (green) &&          \
                  name.b == (blue) && name.a == (alpha),           \
                  "design token value changed: " #name)

MV_DT_COLOR(kColorCanvas,                     0.102f, 0.102f, 0.102f, 1.0f);  // #1A1A1A
MV_DT_COLOR(kColorPlaceholder,                0.10f,  0.10f,  0.12f,  1.0f);
MV_DT_COLOR(kColorOverlayText,                1.0f,   1.0f,   1.0f,   0.85f);
MV_DT_COLOR(kColorHintText,                   0.5f,   0.5f,   0.5f,   0.8f);
MV_DT_COLOR(kColorStatusErrorBg,              0.18f,  0.08f,  0.06f,  0.96f);
MV_DT_COLOR(kColorStatusErrorBorder,          0.88f,  0.38f,  0.24f,  1.0f);
MV_DT_COLOR(kColorStatusErrorText,            0.96f,  0.90f,  0.88f,  1.0f);
MV_DT_COLOR(kColorInfoCardBg,                 0.06f,  0.06f,  0.08f,  0.94f);
MV_DT_COLOR(kColorInfoCardBorder,             0.22f,  0.22f,  0.28f,  0.7f);
MV_DT_COLOR(kColorInfoLabel,                  0.45f,  0.45f,  0.50f,  1.0f);
MV_DT_COLOR(kColorInfoValue,                  0.88f,  0.88f,  0.90f,  1.0f);
MV_DT_COLOR(kColorInfoDim,                    0.35f,  0.35f,  0.38f,  1.0f);
MV_DT_COLOR(kColorOverlayShadow,              0.0f,   0.0f,   0.0f,   0.6f);
MV_DT_COLOR(kColorPlaceholderMin,             0.18f,  0.18f,  0.20f,  1.0f);
MV_DT_COLOR(kColorFavouriteChip,              0.10f,  0.10f,  0.11f,  0.92f);
MV_DT_COLOR(kColorFavouriteHeart,             0.92f,  0.32f,  0.42f,  1.0f);
MV_DT_COLOR(kColorComicErrorBg,               0.20f,  0.12f,  0.12f,  1.0f);
MV_DT_COLOR(kColorComicPlaceholderBg,         0.16f,  0.16f,  0.18f,  1.0f);
MV_DT_COLOR(kColorComicErrorBorder,           0.42f,  0.24f,  0.24f,  1.0f);
MV_DT_COLOR(kColorComicPlaceholderBorder,     0.22f,  0.22f,  0.25f,  1.0f);
MV_DT_COLOR(kColorComicErrorText,             0.88f,  0.72f,  0.72f,  1.0f);
MV_DT_COLOR(kColorComicScrollbarTrack,        0.48f,  0.48f,  0.52f,  0.26f);
MV_DT_COLOR(kColorComicScrollbarThumbActive,  0.78f,  0.78f,  0.84f,  0.96f);
MV_DT_COLOR(kColorComicScrollbarThumbHover,   0.66f,  0.66f,  0.72f,  0.90f);
MV_DT_COLOR(kColorComicScrollbarThumbIdle,    0.50f,  0.50f,  0.56f,  0.72f);
MV_DT_COLOR(kColorComicOverlayBg,             0.08f,  0.08f,  0.09f,  1.0f);
MV_DT_COLOR(kColorComicOverlayBorder,         0.56f,  0.56f,  0.62f,  0.34f);
MV_DT_COLOR(kColorComicOverlayText,           0.91f,  0.91f,  0.93f,  0.94f);
MV_DT_COLOR(kColorAutoscrollZone,             0.10f,  0.10f,  0.12f,  0.58f);
MV_DT_COLOR(kColorAutoscrollRing,             0.72f,  0.72f,  0.78f,  0.72f);
MV_DT_COLOR(kColorAutoscrollArrow,            0.84f,  0.84f,  0.90f,  1.0f);
MV_DT_COLOR(kColorWidthSliderTrack,           0.30f,  0.30f,  0.36f,  0.9f);
MV_DT_COLOR(kColorWidthSliderThumb,           0.62f,  0.62f,  0.70f,  1.0f);
MV_DT_COLOR(kColorTextDim,                    0.55f,  0.55f,  0.60f,  1.0f);
MV_DT_COLOR(kColorCruiseIndicator,            0.55f,  0.62f,  0.72f,  1.0f);
MV_DT_COLOR(kColorSelectionAccent,            0.29f,  0.56f,  1.0f,   1.0f);  // drifted accent blue: do NOT merge
MV_DT_COLOR(kColorNavAccent,                  0.29f,  0.56f,  0.89f,  1.0f);  // drifted accent blue: do NOT merge
MV_DT_COLOR(kColorLabelDefault,               0.82f,  0.82f,  0.85f,  1.0f);
MV_DT_COLOR(kColorPanelBg,                    0.10f,  0.10f,  0.12f,  0.95f);
MV_DT_COLOR(kColorPanelDivider,               0.20f,  0.20f,  0.24f,  1.0f);
MV_DT_COLOR(kColorPanelLabel,                 0.80f,  0.80f,  0.82f,  1.0f);
MV_DT_COLOR(kColorPanelValue,                 0.60f,  0.60f,  0.64f,  1.0f);
MV_DT_COLOR(kColorPanelSelection,             0.20f,  0.40f,  0.70f,  0.35f);
MV_DT_COLOR(kColorPanelSectionTitle,          0.70f,  0.70f,  0.74f,  1.0f);
MV_DT_COLOR(kColorPanelToastBg,               0.15f,  0.15f,  0.18f,  0.92f);
MV_DT_COLOR(kColorPanelToastText,             0.85f,  0.85f,  0.88f,  1.0f);
MV_DT_COLOR(kColorScrollbarThumbActive,       0.58f,  0.58f,  0.64f,  0.95f);
MV_DT_COLOR(kColorScrollbarThumbIdle,         0.40f,  0.40f,  0.45f,  0.70f);
MV_DT_COLOR(kColorBlack,                      0.0f,   0.0f,   0.0f,   1.0f);
MV_DT_COLOR(kColorFilmstripArrow,             0.82f,  0.82f,  0.85f,  1.0f);
MV_DT_COLOR(kColorToolbarBg,                  0.11f,  0.11f,  0.13f,  1.0f);
MV_DT_COLOR(kColorToolbarText,                0.8f,   0.8f,   0.82f,  1.0f);
MV_DT_COLOR(kColorToolbarHoverBg,             0.18f,  0.18f,  0.22f,  1.0f);
MV_DT_COLOR(kColorTitleText,                  0.80f,  0.80f,  0.83f,  1.0f);
MV_DT_COLOR(kColorTitleMenuText,              0.72f,  0.72f,  0.75f,  1.0f);
MV_DT_COLOR(kColorTitleMenuHoverBg,           0.22f,  0.22f,  0.26f,  1.0f);
MV_DT_COLOR(kColorWindowButtonCloseHover,     0.91f,  0.30f,  0.24f,  1.0f);
MV_DT_COLOR(kColorWindowButtonHover,          0.70f,  0.70f,  0.70f,  1.0f);
MV_DT_COLOR(kColorWindowButtonSymbol,         0.88f,  0.88f,  0.88f,  1.0f);
MV_DT_COLOR(kColorBreadcrumbHover,            0.97f,  0.97f,  0.98f,  1.0f);
MV_DT_COLOR(kColorBreadcrumbLine,             0.20f,  0.20f,  0.22f,  1.0f);
MV_DT_COLOR(kColorNavLine,                    0.16f,  0.16f,  0.19f,  1.0f);
MV_DT_COLOR(kColorNavDim,                     0.50f,  0.50f,  0.55f,  1.0f);
MV_DT_COLOR(kColorNavBright,                  0.93f,  0.93f,  0.95f,  1.0f);
MV_DT_COLOR(kColorNavHoverBg,                 0.15f,  0.15f,  0.18f,  1.0f);
MV_DT_COLOR(kColorNavBadge,                   0.42f,  0.62f,  0.85f,  1.0f);
MV_DT_COLOR(kColorNavError,                   0.69f,  0.50f,  0.50f,  1.0f);

#undef MV_DT_COLOR

// ── GDI colors (COLORREF / RGB) ──────────────────────────────
#define MV_DT_GDI_COLOR(name, red, green, blue)                    \
    inline constexpr COLORREF name = RGB((red), (green), (blue));  \
    static_assert(name == RGB((red), (green), (blue)),             \
                  "design token value changed: " #name)

MV_DT_GDI_COLOR(kColorWindowBgGdi,          26,  26,  26);  // #1A1A1A
MV_DT_GDI_COLOR(kColorMenuBgGdi,            32,  32,  36);
MV_DT_GDI_COLOR(kColorMenuSeparatorGdi,     60,  60,  65);
MV_DT_GDI_COLOR(kColorMenuSelectedBgGdi,    50,  50,  55);
MV_DT_GDI_COLOR(kColorMenuTextDisabledGdi, 100, 100, 105);
MV_DT_GDI_COLOR(kColorMenuCheckEnabledGdi, 220, 220, 225);
MV_DT_GDI_COLOR(kColorMenuTextGdi,         230, 230, 235);
MV_DT_GDI_COLOR(kColorMenuShortcutDisabledGdi, 80,  80,  85);
MV_DT_GDI_COLOR(kColorMenuShortcutGdi,     160, 160, 168);
MV_DT_GDI_COLOR(kColorFallbackTextGdi,     128, 128, 128);

#undef MV_DT_GDI_COLOR

// ── Motion / durations ───────────────────────────────────
// Visual-timing tokens only. Behavioral timing (async watchdog, dirwatch
// debounce, cache budgets) deliberately stays where the behavior lives.
#define MV_DT_VALUE(name, value)                                   \
    inline constexpr auto name = (value);                          \
    static_assert(name == (value), "design token value changed: " #name)

MV_DT_VALUE(kDurationTransitionSec, 0.25f);        // grid <-> image transition (Quick Look 实测 233-267ms)
MV_DT_VALUE(kTransitionEntryFastFraction, 0.32f);  // 打开: 前 32% 时间冲到 90% 尺寸
MV_DT_VALUE(kTransitionEntryFastReach, 0.90f);     // 打开: 快速阶段终点尺寸
MV_DT_VALUE(kTransitionExitHoldFraction, 0.60f);   // 关闭: 前 60% 保持全尺寸, 背景揭示网格
MV_DT_VALUE(kDurationFilmstripHandoffSec, 0.30f);  // filmstrip slot handoff
MV_DT_VALUE(kDurationFilmstripHideSec, 3.0f);      // 静止 3s 自动收起
MV_DT_VALUE(kDurationFilmstripRevealSec, 0.25f);   // 鼠标移动升起/收起动画
MV_DT_VALUE(kDurationToastMs, 1000);
MV_DT_VALUE(kDurationSelectionHighlightMs, 1000);
MV_DT_VALUE(kDurationImageDebounceMs, 250);
MV_DT_VALUE(kDurationScrollPauseMs, 80);
MV_DT_VALUE(kDurationRenderRetryMs, 120);
MV_DT_VALUE(kAnimationFrameMs, 16);                // WM_TIMER safety net
MV_DT_VALUE(kScrollbarThumbRadiusFraction, 0.4f);
MV_DT_VALUE(kFavouriteBadgeRadiusFraction, 0.3f);
MV_DT_VALUE(kFavouriteBadgeGlyphSizeFraction, 0.55f);

#undef MV_DT_VALUE

} // namespace mv::dt
