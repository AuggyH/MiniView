#pragma once
// Single source of truth for UI geometry constants (nominal DIP values).
//
// All values are DIP (device-independent pixel) nominal values at 96 DPI.
// Consumers scale by dpi/96 (see dpi_scale) before use in physical pixels.
// The App applies DPI scaling centrally in App::apply_dpi_layout(); the
// Renderer applies it where it draws (m_dpi_y / 96.0f). No product-visible
// value changes with this table — it only names constants that were
// duplicated as magic numbers across app.cpp / renderer.cpp.

#include "design_tokens.h"

namespace mv {
namespace layout {

inline constexpr auto kBaseDpi = dt::kBaseDpi;

// ── Title bar (custom-drawn, replaces system caption) ──
inline constexpr auto kTitleBarHeightDip      = static_cast<int>(dt::kSpace40Dip);    // title bar height
inline constexpr auto kTitleBarPadDip         = dt::kSpaceMdDip; // left padding
inline constexpr auto kTitleBarTitleWidthDip  = dt::kTitleBarTitleWidthDip; // "MinView" title block width
inline constexpr auto kTitleBarTitleGapDip    = dt::kSpaceXsDip;  // title → first menu gap
inline constexpr auto kTitleBarMenuFontSizeDip = dt::kFontSizeMdDip;
inline constexpr auto kTitleBarMenuPadDip     = dt::kSpaceLgDip; // horizontal padding per menu item
inline constexpr auto kTitleBarButtonWidthDip = dt::kTitleBarButtonWidthDip; // min/max/close button width

// ── Thumbnail grid ──
inline constexpr auto kThumbCellDip          = dt::kThumbCellDip;   // display cell → column calc
inline constexpr auto kThumbSizeDip          = dt::kThumbSizeDip;   // decode resolution (2x supersampling)
inline constexpr auto kThumbGapHDip          = static_cast<int>(dt::kSpaceSmDip); // horizontal gap
inline constexpr auto kThumbGapVDip          = static_cast<int>(dt::kSpaceLgDip); // vertical gap
inline constexpr auto kThumbPadDip           = static_cast<int>(dt::kSpaceSmDip); // uniform padding
inline constexpr auto kGridLabelHeightDip    = dt::kGridLabelHeightDip; // filename label block height
inline constexpr auto kThumbCornerRadiusDip  = dt::kSpaceXsDip;  // thumbnail corner radius

// ── Right side panel ──
inline constexpr auto kPanelWidthDip             = dt::kPanelWidthDip;
inline constexpr auto kPanelLabelColumnWidthDip  = dt::kPanelLabelColumnWidthDip; // label column width

// ── Left navigation panel (Issue #5 P2) ──
inline constexpr auto kNavPanelWidthDip          = static_cast<int>(dt::kSize240Dip); // fixed width (D-9)
inline constexpr auto kNavBreadcrumbBarHeightDip = dt::kSize36Dip; // breadcrumb strip height (panel top + grid content top)
inline constexpr auto kNavTabHeightDip           = dt::kSize28Dip; // 「目录」「收藏」tab row height
inline constexpr auto kNavRowHeightDip           = dt::kSize28Dip; // directory tree row height
inline constexpr auto kNavIndentDip              = dt::kSpaceLgDip; // tree depth indent per level
inline constexpr auto kNavArrowWidthDip          = dt::kSpace20Dip; // expand/collapse arrow hit zone
inline constexpr auto kNavStatsHeightDip         = dt::kSpaceXlDip; // bottom collection stats row height
inline constexpr auto kNavScrollbarWidthDip      = dt::kSpace6Dip;  // tree scrollbar thumb width
inline constexpr auto kNavToggleButtonWidthDip   = dt::kSize44Dip; // album view-mode toggle
inline constexpr auto kNavFontSizeDip            = dt::kFontSizeMdDip; // tree/tab/breadcrumb font size
inline constexpr auto kNavSmallFontSizeDip       = dt::kFontSizeXsDip; // counts/stats font size
inline constexpr auto kNavPadDip                 = dt::kSpaceSmDip;  // panel inner padding
inline constexpr auto kNavBreadcrumbGapDip       = dt::kSpaceXsDip;  // breadcrumb segment gap

// ── Scrollbar ──
inline constexpr auto kScrollbarZoneDip = static_cast<int>(dt::kSpace20Dip); // grid scrollbar hit zone width

// ── Filmstrip (large-image bottom strip, Issue #5 P1) ──
inline constexpr auto kFilmstripHeightDip      = dt::kFilmstripHeightDip;  // strip height (8px grid: 64+8+8)
inline constexpr auto kFilmstripPadVDip        = dt::kSpaceSmDip;   // top/bottom padding
inline constexpr auto kFilmstripThumbHeightDip = dt::kSize64Dip;  // max thumb height (8px grid)
inline constexpr auto kFilmstripGapDip         = dt::kSpaceSmDip;   // horizontal gap
inline constexpr auto kFilmstripCurrentScale   = dt::kFilmstripCurrentScale;  // current item zoom
inline constexpr auto kFilmstripBorderDip      = dt::kFilmstripBorderDip;   // current border
inline constexpr auto kFilmstripIndicatorDip   = dt::kSpaceXsDip;   // current indicator
inline constexpr auto kFilmstripArrowZoneDip   = dt::kSpaceXlDip;  // edge arrow zone
inline constexpr auto kFilmstripHoverZoneDip   = dt::kSpaceXlDip;  // fullscreen hover band
inline constexpr auto kFilmstripHideDelaySeconds = dt::kDurationFilmstripHideSec; // fullscreen auto-hide
inline constexpr auto kFilmstripMaxAspect      = dt::kFilmstripMaxAspect;  // clamp w/h ratio

// ── Window ──
inline constexpr auto kDefaultWindowWidthDip  = dt::kDefaultWindowWidthDip;
inline constexpr auto kDefaultWindowHeightDip = dt::kDefaultWindowHeightDip;

// Compile-time guard: int layout constants that reuse float dt tokens must
// stay value-identical after the type-preserving cast.
static_assert(kTitleBarHeightDip == dt::kSpace40Dip, "layout token drifted: kTitleBarHeightDip");
static_assert(kThumbGapHDip == dt::kSpaceSmDip, "layout token drifted: kThumbGapHDip");
static_assert(kThumbGapVDip == dt::kSpaceLgDip, "layout token drifted: kThumbGapVDip");
static_assert(kThumbPadDip == dt::kSpaceSmDip, "layout token drifted: kThumbPadDip");
static_assert(kNavPanelWidthDip == dt::kSize240Dip, "layout token drifted: kNavPanelWidthDip");
static_assert(kScrollbarZoneDip == dt::kSpace20Dip, "layout token drifted: kScrollbarZoneDip");

// DPI scale factor (dpi / 96). dpi is always a positive physical value
// (GetDpiForWindow / GetDpiForSystem / renderer dpi state).
inline float dpi_scale(float dpi) noexcept { return dpi / kBaseDpi; }

} // namespace layout
} // namespace mv
