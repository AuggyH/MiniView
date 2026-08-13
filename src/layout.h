#pragma once
// Single source of truth for UI geometry constants (nominal DIP values).
//
// All values are DIP (device-independent pixel) nominal values at 96 DPI.
// Consumers scale by dpi/96 (see dpi_scale) before use in physical pixels.
// The App applies DPI scaling centrally in App::apply_dpi_layout(); the
// Renderer applies it where it draws (m_dpi_y / 96.0f). No product-visible
// value changes with this table — it only names constants that were
// duplicated as magic numbers across app.cpp / renderer.cpp.

namespace mv {
namespace layout {

inline constexpr float kBaseDpi = 96.0f;

// ── Title bar (custom-drawn, replaces system caption) ──
inline constexpr int   kTitleBarHeightDip      = 40;    // title bar height
inline constexpr float kTitleBarPadDip         = 12.0f; // left padding
inline constexpr float kTitleBarTitleWidthDip  = 68.0f; // "MinView" title block width
inline constexpr float kTitleBarTitleGapDip    = 4.0f;  // title → first menu gap
inline constexpr float kTitleBarMenuFontSizeDip = 12.0f;
inline constexpr float kTitleBarMenuPadDip     = 16.0f; // horizontal padding per menu item
inline constexpr float kTitleBarButtonWidthDip = 46.0f; // min/max/close button width

// ── Thumbnail grid ──
inline constexpr int   kThumbCellDip          = 200;   // display cell → column calc
inline constexpr int   kThumbSizeDip          = 400;   // decode resolution (2x supersampling)
inline constexpr int   kThumbGapHDip          = 8;     // horizontal gap
inline constexpr int   kThumbGapVDip          = 16;    // vertical gap
inline constexpr int   kThumbPadDip           = 8;     // uniform padding
inline constexpr int   kGridLabelHeightDip    = 42;    // filename label block height
inline constexpr float kThumbCornerRadiusDip  = 4.0f;  // thumbnail corner radius

// ── Right side panel ──
inline constexpr int   kPanelWidthDip             = 280;
inline constexpr float kPanelLabelColumnWidthDip  = 70.0f; // label column width

// ── Left navigation panel (Issue #5 P2) ──
inline constexpr int   kNavPanelWidthDip          = 240;   // fixed width (D-9)
inline constexpr float kNavBreadcrumbBarHeightDip = 36.0f; // breadcrumb strip height (panel top + grid content top)
inline constexpr float kNavTabHeightDip           = 28.0f; // 「目录」「收藏」tab row height
inline constexpr float kNavRowHeightDip           = 28.0f; // directory tree row height
inline constexpr float kNavIndentDip              = 16.0f; // tree depth indent per level
inline constexpr float kNavArrowWidthDip          = 20.0f; // expand/collapse arrow hit zone
inline constexpr float kNavStatsHeightDip         = 24.0f; // bottom collection stats row height
inline constexpr float kNavScrollbarWidthDip      = 6.0f;  // tree scrollbar thumb width
inline constexpr float kNavFontSizeDip            = 12.0f; // tree/tab/breadcrumb font size
inline constexpr float kNavSmallFontSizeDip       = 10.0f; // counts/stats font size
inline constexpr float kNavPadDip                 = 8.0f;  // panel inner padding
inline constexpr float kNavBreadcrumbGapDip       = 4.0f;  // breadcrumb segment gap

// ── Scrollbar ──
inline constexpr int kScrollbarZoneDip = 20; // grid scrollbar hit zone width

// ── Window ──
inline constexpr int kDefaultWindowWidthDip  = 1400;
inline constexpr int kDefaultWindowHeightDip = 900;

// DPI scale factor (dpi / 96). dpi is always a positive physical value
// (GetDpiForWindow / GetDpiForSystem / renderer dpi state).
inline float dpi_scale(float dpi) noexcept { return dpi / kBaseDpi; }

} // namespace layout
} // namespace mv
