// Responsive layout engine (Phase 3). A PURE function: given the terminal
// size, the panel list and a couple of preferences, it returns a rect and a
// render mode for every panel. No FTXUI, no I/O, no clock — so it is
// trivially unit-testable and can be re-run the instant a resize event lands
// (and is cheap enough to re-run on every one).
//
// The renderer (frame.cpp) draws each panel into the rect it is given, in the
// mode it is told to use. This file only decides WHERE and HOW BIG — it never
// draws a pixel and never reads a metric.
//
// Coordinate convention: x is a terminal COLUMN (0 = left edge). y is an
// OFFSET FROM THE TOP OF THE CONTENT AREA (0 = the first row below the
// header). The header is row 0 and the footer is the last row of the
// terminal; the content area is the content_height rows in between. The
// renderer maps a content y to a terminal row by adding 1 (and, while
// scrolling, subtracting the scroll offset).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "ui/panel_spec.hpp"

namespace geth::ui {

// How a panel is rendered inside its rect.
enum class PanelMode {
  kFull,    // border + graph (>=4 rows) + legend rows + info rows
  kCompact, // values in the top border, graph >=3, no info rows; legend only if
            // the title truncated and a spare row exists
  kTight,   // values in the top border, graph >=2, abbreviated axis, top/bottom
            // labels only
};

// Smallest panel height (borders included) for each mode: Full is width/panel
// dependent (see full_height), Compact and Tight are fixed.
constexpr int kCompactMinH = 5;  // border 2 + graph 3
constexpr int kTightMinH   = 4;  // border 2 + graph 2

struct LayoutRect {
  int x = 0;   // terminal column
  int y = 0;   // offset from the top of the content area (0-based)
  int w = 0;   // panel width (terminal columns, border included)
  int h = 0;   // panel height (terminal rows, border included)
  int row = 0; // which grid row this panel is in (scroll snapping)
  bool operator==(const LayoutRect& o) const {
    return x == o.x && y == o.y && w == o.w && h == o.h && row == o.row;
  }
};

struct PanelLayout {
  LayoutRect rect;
  PanelMode mode = PanelMode::kFull;
};

struct Layout {
  std::vector<PanelLayout> panels;  // parallel to the input panels (all of them)
  int columns = 1;                  // chosen column count
  // Heights of the VISIBLE rows: every row when not scrolling, or the k
  // stretched window rows when scrolling. Always sums to content_height, so
  // the visible area is tiled exactly (no blank rows).
  std::vector<int> row_heights;
  int content_height = 0;           // term_h - header - footer
  int page_height = 0;              // == content_height (the visible area fills)
  int total_rows = 0;               // rows in the full grid; > rows() while scrolling
  bool scrolling = false;           // more grid rows than fit in the viewport
  // The visible rows: all rows when not scrolling, the window (k rows) when
  // scrolling. A panel's visible row is rect.row (non-scroll) or rect.row -
  // offset (scroll); the renderer maps it to a terminal row via the prefix
  // sum of row_heights.
  int rows() const { return static_cast<int>(row_heights.size()); }
};

struct LayoutPrefs {
  std::optional<int> columns;  // --columns N: pin the column count
};

// The visible panels of a page (Phase 4: hidden ones are laid out as if
// absent). Every layout entry point filters through this so the layout
// engine, the report and the cache can never disagree.
std::vector<PanelSpec> visible_panels(std::span<const PanelSpec> page);

// The layout for one frame. `panels` is the page in reading order (the order
// they flow into the grid, left to right, top to bottom).
Layout compute_layout(int term_w, int term_h, std::span<const PanelSpec> panels,
                      const LayoutPrefs& prefs);

// For --layout-report (Part C): the scored candidate for EACH feasible column
// count, using the EXACT same scoring as compute_layout (so the report always
// agrees with what the UI will actually pick). The chosen candidate is the
// best_full / best_mixed winner, i.e. the one compute_layout returns.
struct CandidateReport {
  int columns = 0;               // this candidate's column count
  bool valid = false;            // every panel fits at least Tight
  bool all_full = false;         // and every one of them in Full mode
  int degraded = 0;              // panels not in Full mode (Compact + Tight)
  int min_graph_h = 0;           // smallest graph height across the panels
  long long total_graph_area = 0;  // sum of graph_h x graph_w
};
std::vector<CandidateReport> score_layout_candidates(int term_w, int term_h,
                                                     std::span<const PanelSpec> panels,
                                                     const LayoutPrefs& prefs);

// Scrolling helpers (used by both the renderer and the event loop).
// The largest scroll offset (the index of the topmost visible row):
// total_rows - k while scrolling, 0 otherwise.
int max_top_row(const Layout& lo, int content_h);
// How many rows the viewport shows at once: the window size k while
// scrolling, all rows otherwise.
int rows_fitting(const Layout& lo, int content_h);

// ---------------------------------------------------------------------------
// Geometry helpers — exported for the unit tests and REUSED by frame.cpp so
// the renderer and the layout engine can never disagree about how tall a
// panel needs to be or how wide its graph is.
// ---------------------------------------------------------------------------

// Display columns of the widest y-axis tick label for this scale kind, in the
// full ("100 %", "512 MiB", "128 MiB/s") or the abbreviated ("100%", "24G",
// "1.5M/s") form. The abbreviated form is narrower, so a panel can be narrower
// and more columns fit.
int axis_label_width(ScaleSpec::Kind k);    // full form
int abbrev_axis_width(ScaleSpec::Kind k);   // abbreviated form

// A panel uses abbreviated axis labels when its INNER width is below this, in
// any mode. That narrows the axis and lowers the minimum panel width.
constexpr int kAbbrevInnerW = 60;
inline bool uses_abbrev_labels(int inner_w) { return inner_w < kAbbrevInnerW; }

// Smallest panel width whose graph meets the mode's minimum (32 for
// Full/Compact, 24 for Tight), using the abbreviated axis (the narrowest case):
// W = min_graph_w(mode) + abbrev_axis + 2 (the " ┤" separator) + 2 (borders).
int min_panel_width(const PanelSpec& p, PanelMode mode);

// Legend columns at an interior width: 3 at >=96, 2 at >=56, else 1.
int legend_cols(int inner_w);

// Rows the legend needs at an interior width (0 for a legend-less panel).
int legend_rows(const PanelSpec& p, int inner_w);

// Panel height (borders included) Full mode needs at a given panel width.
int full_height(const PanelSpec& p, int panel_w);

// Smallest Compact-mode panel height (width-independent): border + 3 graph
// rows, no legend (the legend is a render-time extra, shown only when the
// title truncated and a spare row exists).
int compact_height();   // == kCompactMinH
// Smallest Tight-mode panel height (width-independent): border + 2 graph rows.
int tight_height();     // == kTightMinH

// Graph height (rows) of a panel given its rect, per mode.
int full_graph_h(const PanelSpec& p, int panel_w, int panel_h);
int compact_graph_h(int panel_h);
int tight_graph_h(int panel_h);

// Graph width (columns) of a panel given its width, full or abbreviated axis.
int graph_w(const PanelSpec& p, int panel_w, bool abbreviated);

// The best mode the panel can use at a rect of `w`x`h`; false when it cannot
// even fit Tight. Used by the selector and by the renderer (in scroll mode the
// visible height — and therefore the mode — depends on the scroll offset).
bool best_mode(const PanelSpec& p, int w, int h, PanelMode& mode);

}  // namespace geth::ui
