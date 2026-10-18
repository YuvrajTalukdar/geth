// The graph data path and the graph-rect geometry, shared by the text
// renderer (frame.cpp) and the pixel backends (graph_image.cpp,
// graph_backends.cpp) so the two can never disagree.
//
// Data path: the in-window points of a series from the SeriesStore
// (load_live), decimation to one point per dot column (decimate), and
// interpolation at any time (value_at).
//
// Geometry: for a panel box (x0,y0,W,H) and mode, the interior layout —
// legend/info rows, the graph rect (gx,gy,Wg,Hg), the tick fractions and
// labels, the dot-column width — plus the pixel mapping for the rasterizer.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "core/series_store.hpp"
#include "ui/layout.hpp"      // PanelMode, legend_cols, uses_abbrev_labels
#include "ui/panel_spec.hpp"  // PanelSpec, ScaleSpec

namespace geth::ui {

// Column count of a UTF-8 string (every glyph GETH emits is one cell wide).
inline int utf8_cols(const std::string& s) {
  int n = 0, i = 0;
  while (i < (int)s.size()) {
    const unsigned char c = s[i];
    if (c < 0x80) i += 1;
    else if ((c >> 5) == 6) i += 2;
    else if ((c >> 4) == 14) i += 3;
    else if ((c >> 3) == 30) i += 4;
    else i += 1;  // invalid byte: treat as one cell
    ++n;
  }
  return n;
}

// In-window sample points (oldest -> newest) of one series, as a zero-copy
// view into caller-owned `t`/`v`.
struct RawWindow {
  const int64_t* t = nullptr;
  const float* v = nullptr;
  std::size_t n = 0;
  int64_t t_start = 0, t_end = 0;
  double span_ns = 0;
  bool valid() const { return t && v && n > 0 && t_start < t_end; }
};

// Fill `t`/`v` (and point `w` at them) with one series' in-window points
// from the live store. The window is the newest `window_ns` of history
// (the graph time window: 30 s .. 10 m, cycled with t); the ring holds a
// full hour, so every window is a slice of it.
void load_live(RawWindow& w, std::vector<int64_t>& t, std::vector<float>& v,
               const SeriesStore& store, const SeriesId& id, int64_t window_ns);

// One point per dot column (plus the ends): enough for line interpolation at
// any width without iterating every sample of the 60 s window.
struct Decimated {
  std::vector<int64_t> t;
  std::vector<float> v;
};

Decimated decimate(const RawWindow& w, int dot_cols);

// The value of a series at time `t` (ns), or NaN when it falls in a gap.
// Linear interpolation between the decimated samples keeps the line
// continuous at any width.
double value_at(const Decimated& d, double t);

// The largest finite value over the windows (for auto-scaling).
double visible_max(const std::vector<RawWindow>& raws);

// Compact tick labels: no trailing zeros, a space before the unit, °C for
// temperature — "100 %", "75 %", "0 %", "100 °C", "32 GiB", "30 W", "5 MiB/s".
// The legend keeps the full-precision formatters (format::bytes/watts/...).
std::string tick_label(ScaleSpec::Kind k, double f, double vmax);
// Abbreviated tick labels for narrow panels (inner width < 60): single-letter
// units, no spaces — "100%", "100°", "24G", "20W", "1.5M/s". The layout
// engine budgets the axis with the SAME rule (uses_abbrev_labels), so the
// two can never disagree.
std::string abbrev_tick_label(ScaleSpec::Kind k, double f, double vmax);

// The layout of a panel's interior: the legend/info rows, the graph rect,
// and the ticks. Computed once per panel per frame by the text renderer and
// reused by the pixel image placement (which must draw exactly where the
// text reserved), so the two can never disagree.
struct GraphGeom {
  // The panel box (screen cells).
  int x0 = 0, y0 = 0, W = 0, H = 0;
  // The interior (inside the 1-cell border).
  int ix0 = 0, iy0 = 0, iw = 0, ih = 0;
  // The legend: row count, columns per row, and the series indices (into
  // PanelSpec::series) that appear in it.
  int legend_rows = 0, ipr = 1;
  std::vector<int> legend_idx;
  int info_n = 0;
  // The graph rect in screen cells: top-left (gx,gy), Wg x Hg.
  // have_graph is false when there is no room or nothing to draw.
  bool have_graph = false;
  int gx = 0, gy = 0, Wg = 0, Hg = 0;
  int label_w = 0;   // the widest tick label's width (L)
  int dot_cols = 0;  // 2 * Wg — the line's horizontal resolution
  std::vector<double> fracs;       // the tick fractions, top -> bottom
  std::vector<std::string> ticks;  // their labels (same order)
  double vmax = 0;

  // The row within the graph (0-based) that carries fracs[i].
  int tick_row(std::size_t i) const {
    return static_cast<int>(std::lround((1.0 - fracs[i]) * (Hg - 1)));
  }
  // The pixel y (inside an image whose top-left sits at (gx,gy)) of value v:
  // v = vmax is cell_h/2 below the top edge, v = 0 is cell_h/2 +
  // (Hg-1)*cell_h. Continuous (the rasterizer's AA wants sub-pixel).
  double value_y_px(double v, int cell_h) const {
    return cell_h / 2.0 + (1.0 - v / vmax) * (Hg - 1) * cell_h;
  }
  // The pixel y of the gridline for fracs[i] — the same row as the text tick
  // label.
  int tick_y_px(std::size_t i, int cell_h) const {
    return cell_h / 2 + tick_row(i) * cell_h;
  }
};

// Computes the interior layout of the box (x0,y0,W,H) in `mode` for panel
// `p` with y-max `vmax`. `title_truncated` (from the border pass) triggers
// the one-line legend rescue in Compact. No graph (have_graph = false) when
// the interior is too small.
GraphGeom compute_graph_geom(int x0, int y0, int W, int H, PanelMode mode,
                             const PanelSpec& p, bool title_truncated,
                             double vmax);

}  // namespace geth::ui
