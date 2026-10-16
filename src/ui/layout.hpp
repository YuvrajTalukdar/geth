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
  kFull,    // the current look: border + graph (>=4 rows) + legend rows + info rows
  kCompact, // key values in the top border, one legend line, no info rows, graph >=3
};

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
  std::vector<PanelLayout> panels;  // parallel to the input panels
  int columns = 1;                  // chosen column count
  std::vector<int> row_heights;     // one entry per row (top -> bottom)
  int content_height = 0;           // term_h - header - footer
  int page_height = 0;              // sum(row_heights); > content_height iff scrolling
  bool scrolling = false;           // the page is taller than the content area
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
  bool valid = false;            // every panel fits in Full or Compact
  bool all_full = false;         // and every one of them in Full mode
  int min_graph_h = 0;           // smallest graph height across the panels
  long long total_graph_area = 0;  // sum of graph_h x graph_w
};
std::vector<CandidateReport> score_layout_candidates(int term_w, int term_h,
                                                     std::span<const PanelSpec> panels,
                                                     const LayoutPrefs& prefs);

// Scrolling helpers (used by both the renderer and the event loop).
// The largest top_row (row index) that keeps the bottom of the page from
// scrolling past the bottom of the content area; 0 when not scrolling.
int max_top_row(const Layout& lo, int content_h);
// How many rows (from the top) fit in a content area `content_h` rows tall.
int rows_fitting(const Layout& lo, int content_h);

// ---------------------------------------------------------------------------
// Geometry helpers — exported for the unit tests and REUSED by frame.cpp so
// the renderer and the layout engine can never disagree about how tall a
// panel needs to be or how wide its graph is.
// ---------------------------------------------------------------------------

// Display columns of the widest y-axis tick label for this scale kind.
// Conservative per-kind maxima of the compact formatters in format.hpp:
//   "100 %" (5), "120 °C" (6), "512 MiB" (7), "1.5 kW" (6), "128 MiB/s" (9).
int axis_label_width(ScaleSpec::Kind k);

// Smallest panel width whose graph still meets the 32-column minimum:
// W = graph_w(32) + axis_label + 2 (the " ┤" separator) + 2 (borders).
int min_panel_width(const PanelSpec& p);

// Legend columns at an interior width: 3 at >=96, 2 at >=56, else 1.
int legend_cols(int inner_w);

// Rows the legend needs at an interior width (0 for a legend-less panel).
int legend_rows(const PanelSpec& p, int inner_w);

// Panel height (borders included) Full mode needs at a given panel width.
int full_height(const PanelSpec& p, int panel_w);

// Panel height Compact mode needs (width-independent): border + 3 graph rows
// + one legend line, no info rows.
int compact_height();

// Graph height (rows) of a Full-mode panel given its rect.
int full_graph_h(const PanelSpec& p, int panel_w, int panel_h);
// Graph height (rows) of a Compact-mode panel given its rect.
int compact_graph_h(int panel_h);

// Graph width (columns) of a panel given its width.
int graph_w(const PanelSpec& p, int panel_w);

}  // namespace geth::ui
