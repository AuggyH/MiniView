#pragma once
// Phase 1 design tokens: colors only (no spacing/font/radius/motion yet).
// Header-only, zero external dependencies, C++20 inline constexpr.
// D2D call sites use dt::d2d(dt::kColorX); GDI call sites use dt::kColorXGdi.

#include <windows.h>
#include <d2d1.h>

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

} // namespace mv::dt
