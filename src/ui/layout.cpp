#include "ui/layout.hpp"

#include <algorithm>

namespace geth::ui {

std::vector<PanelSpec> visible_panels(std::span<const PanelSpec> page) {
  std::vector<PanelSpec> out;
  out.reserve(page.size());
  for (const auto& p : page)
    if (p.visible) out.push_back(p);
  return out;
}

namespace {

constexpr int kHeaderRows = 1;        // the title line
constexpr int kFooterRows = 1;        // the key-hints line
constexpr int kBorderRows = 2;        // top + bottom border of a panel
constexpr int kFullGraphMin = 4;      // Full mode: the graph needs >=4 rows
// (Compact/Tight graph minima live in kCompactMinH / kTightMinH in the header:
//  border 2 + graph 3, border 2 + graph 2.)
constexpr int kFullCompactGraphW = 32; // minimum graph width, Full/Compact
constexpr int kTightGraphW = 24;      // minimum graph width, Tight

// Split `total` into `n` parts, sharing equally with the leftover going to
// the FIRST parts (top rows / left columns, per spec).
std::vector<int> split_even(int total, int n) {
  std::vector<int> out(n > 0 ? n : 0);
  if (n <= 0 || total <= 0) return out;
  const int base = total / n;
  const int extra = total % n;
  for (int i = 0; i < n; ++i) out[i] = base + (i < extra ? 1 : 0);
  return out;
}

// A scored candidate: one column count, fully laid out.
struct Candidate {
  bool valid = false;      // every panel fits at least Tight
  bool all_full = false;   // and every one of them in Full mode
  int degraded = 0;        // panels not in Full mode (Compact + Tight)
  int min_graph_h = 0;     // smallest graph height across the panels
  long long total_graph_area = 0;  // sum of graph_h x graph_w
  Layout layout;
};

// True when `a` beats `b`: a taller smallest graph first, then more total
// graph area, then fewer degraded (non-Full) panels. Ties keep the earlier
// (fewer-column) candidate because the caller only replaces on a strict win.
// ("No scrolling" is always true for a feasible candidate — rows are split to
// exactly content_height — so it is not a differentiator here.)
bool better(const Candidate& a, const Candidate& b) {
  if (a.min_graph_h != b.min_graph_h) return a.min_graph_h > b.min_graph_h;
  if (a.total_graph_area != b.total_graph_area)
    return a.total_graph_area > b.total_graph_area;
  return a.degraded < b.degraded;
}

// Lay out one fixed column count and score it. The rects are content-relative
// (y from the top of the content area). Fills cand.layout whether or not the
// candidate is valid, so the caller can inspect a best-fit even when it must
// fall back to scrolling.
Candidate build_candidate(int term_w, int content_h, int c,
                          std::span<const PanelSpec> panels) {
  Candidate cand;
  const int n = static_cast<int>(panels.size());
  Layout& lo = cand.layout;
  lo.columns = c;
  lo.content_height = content_h;
  lo.panels.assign(n, {});
  if (n == 0 || c <= 0) return cand;

  const int rows = (n + c - 1) / c;
  lo.row_heights = split_even(content_h, rows);
  lo.page_height = content_h;  // rows sum to exactly content_h -> no scrolling
  lo.total_rows = rows;

  // Column widths/offsets for a FULL row (c panels); leftover to the left.
  const std::vector<int> col_w = split_even(term_w, c);
  std::vector<int> col_x(c, 0);
  for (int j = 1; j < c; ++j) col_x[j] = col_x[j - 1] + col_w[j - 1];

  // The last row, when it holds fewer than c panels, stretches to the full
  // terminal width, divided among the panels that are actually in it.
  const int last_k = n - (rows - 1) * c;  // 1..c
  const bool last_partial = last_k < c;
  std::vector<int> last_w, last_x;
  if (last_partial) {
    last_w = split_even(term_w, last_k);
    last_x.assign(last_k, 0);
    for (int j = 1; j < last_k; ++j) last_x[j] = last_x[j - 1] + last_w[j - 1];
  }

  int y = 0;
  int min_h = 0;
  long long area = 0;
  bool all_full = true, all_fit = true;
  int degraded = 0;
  for (int r = 0; r < rows; ++r) {
    const bool is_last = (r == rows - 1);
    const int k_in_row = is_last ? last_k : c;
    for (int m = 0; m < k_in_row; ++m) {
      const int i = r * c + m;
      if (i >= n) break;
      const PanelSpec& p = panels[i];
      const int w = (is_last && last_partial) ? last_w[m] : col_w[m];
      const int x = (is_last && last_partial) ? last_x[m] : col_x[m];
      const int h = lo.row_heights[r];
      PanelLayout& pl = lo.panels[i];
      pl.rect = LayoutRect{x, y, w, h, r};

      // Best mode the panel can use at this size; if it can't even fit Tight
      // the candidate is infeasible.
      PanelMode mode;
      if (best_mode(p, w, h, mode)) {
        pl.mode = mode;
        if (mode != PanelMode::kFull) { all_full = false; ++degraded; }
        const int gh = (mode == PanelMode::kFull) ? full_graph_h(p, w, h)
                         : (mode == PanelMode::kCompact) ? compact_graph_h(h)
                                                         : tight_graph_h(h);
        const int gw = graph_w(p, w, uses_abbrev_labels(w - kBorderRows));
        if (min_h == 0 || gh < min_h) min_h = gh;
        area += static_cast<long long>(gh) * gw;
      } else {
        all_fit = false;
        pl.mode = PanelMode::kTight;  // placeholder; the candidate is invalid
      }
    }
    y += lo.row_heights[r];
  }
  cand.valid = all_fit;
  cand.all_full = all_fit && all_full;
  cand.degraded = all_fit ? degraded : 0;
  cand.min_graph_h = all_fit ? min_h : 0;
  cand.total_graph_area = all_fit ? area : 0;
  return cand;
}

// Scroll fallback: used only when NO column count lets every panel fit at
// least Tight without scrolling. It uses c_max columns (the fewest rows) and
// shows a window of k = content_h / tight_height() consecutive rows stretched
// to fill the viewport exactly (split_even) — so no blank rows are left. The
// renderer maps a visible panel (grid row r in [offset, offset+k)) to a
// terminal row via the prefix sum of row_heights and recomputes its height and
// mode there (both depend on the offset); this stores the grid position plus
// the offset-0 representation for the report.
Layout build_scroll(int term_w, int content_h, int c,
                    std::span<const PanelSpec> panels) {
  Layout lo;
  const int n = static_cast<int>(panels.size());
  const int total_rows = (n + c - 1) / c;
  const int k = std::max(1, content_h / tight_height());  // visible rows
  lo.columns = c;
  lo.content_height = content_h;
  lo.total_rows = total_rows;
  lo.scrolling = true;
  lo.row_heights = split_even(content_h, k);  // the k visible rows
  lo.page_height = content_h;                 // the visible area fills exactly
  lo.panels.assign(n, {});

  const std::vector<int> col_w = split_even(term_w, c);
  std::vector<int> col_x(c, 0);
  for (int j = 1; j < c; ++j) col_x[j] = col_x[j - 1] + col_w[j - 1];
  const int last_k = n - (total_rows - 1) * c;
  const bool last_partial = last_k < c;
  std::vector<int> last_w, last_x;
  if (last_partial) {
    last_w = split_even(term_w, last_k);
    last_x.assign(last_k, 0);
    for (int j = 1; j < last_k; ++j) last_x[j] = last_x[j - 1] + last_w[j - 1];
  }

  int y = 0;
  for (int r = 0; r < total_rows; ++r) {
    const bool is_last = (r == total_rows - 1);
    const int k_in_row = is_last ? last_k : c;
    // Offset-0 height: the first k rows use the stretched heights; the rest a
    // placeholder (the renderer recomputes visible heights from the offset).
    const int h = (r < k) ? lo.row_heights[r] : tight_height();
    for (int m = 0; m < k_in_row; ++m) {
      const int i = r * c + m;
      if (i >= n) break;
      const int w = (is_last && last_partial) ? last_w[m] : col_w[m];
      const int x = (is_last && last_partial) ? last_x[m] : col_x[m];
      lo.panels[i].rect = LayoutRect{x, y, w, h, r};
      PanelMode mode;
      lo.panels[i].mode =
          best_mode(panels[i], w, h, mode) ? mode : PanelMode::kTight;
    }
    y += h;
  }
  return lo;
}

// The feasible column range: c in [c_lo, c_hi]. c_hi = c_max, the largest count
// where EVERY panel still meets its minimum width (a panel's narrowest column
// is the base (rightmost) column of a full row, width term_w / c; the last row
// is stretched wider, so it never binds). --columns N pins the range to a
// single (clamped) value. Shared by compute_layout and the --layout-report
// scorer so the two always consider the same candidates.
void column_range(int n, int term_w, std::span<const PanelSpec> panels,
                  const LayoutPrefs& prefs, int& c_max, int& c_lo, int& c_hi) {
  // c_max uses the Tight minimum width (the smallest mode), the most columns
  // that can ever be feasible; the selector then decides which mode each panel
  // actually gets at that width.
  int need = 0;
  for (const auto& p : panels)
    need = std::max(need, min_panel_width(p, PanelMode::kTight));
  c_max = std::max(1, std::min(n, need > 0 ? term_w / need : n));
  c_lo = 1;
  c_hi = c_max;
  if (prefs.columns) {
    const int pc = std::clamp(*prefs.columns, 1, c_max);
    c_lo = c_hi = pc;
  }
}

}  // namespace

// ---- geometry helpers -------------------------------------------------------

int axis_label_width(ScaleSpec::Kind k) {
  switch (k) {
    case ScaleSpec::Kind::kPercent: return 5;     // "100 %"
    case ScaleSpec::Kind::kTemperature: return 6;  // "120 °C"
    case ScaleSpec::Kind::kAbsolute: return 7;    // "512 MiB", "1.5 GiB"
    case ScaleSpec::Kind::kWatts: return 6;       // "1.5 kW"
    case ScaleSpec::Kind::kBytesPerSec: return 9;  // "128 MiB/s"
  }
  return 5;
}

int abbrev_axis_width(ScaleSpec::Kind k) {
  switch (k) {
    case ScaleSpec::Kind::kPercent: return 4;     // "100%"
    case ScaleSpec::Kind::kTemperature: return 4; // "100°"
    case ScaleSpec::Kind::kAbsolute: return 4;    // "24G", "768K", "1.5T"
    case ScaleSpec::Kind::kWatts: return 5;       // "20W", "1.5kW"
    case ScaleSpec::Kind::kBytesPerSec: return 6; // "768K/s", "1.5M/s"
  }
  return 4;
}

int min_panel_width(const PanelSpec& p, PanelMode mode) {
  const int mgw = (mode == PanelMode::kTight) ? kTightGraphW : kFullCompactGraphW;
  // At the narrowest width a panel gets, the inner width is < kAbbrevInnerW, so
  // abbreviated labels are used (the narrowest axis).
  return mgw + abbrev_axis_width(p.y_scale.kind) + 2 + kBorderRows;
}

int legend_cols(int inner_w) {
  if (inner_w >= 96) return 3;
  if (inner_w >= 56) return 2;
  return 1;
}

int legend_rows(const PanelSpec& p, int inner_w) {
  int n = 0;
  for (const auto& s : p.series)
    if (s.in_legend) ++n;
  if (n == 0) return 0;
  const int ipr = legend_cols(inner_w);
  return (n + ipr - 1) / ipr;
}

int full_height(const PanelSpec& p, int panel_w) {
  const int iw = panel_w - kBorderRows;
  return kBorderRows + kFullGraphMin + legend_rows(p, iw) +
         static_cast<int>(p.info_rows.size());
}

int compact_height() { return kCompactMinH; }  // border 2 + graph 3, no legend

int tight_height() { return kTightMinH; }      // border 2 + graph 2

int full_graph_h(const PanelSpec& p, int panel_w, int panel_h) {
  const int iw = panel_w - kBorderRows;
  const int ih = panel_h - kBorderRows;
  return ih - legend_rows(p, iw) - static_cast<int>(p.info_rows.size());
}

// Compact and Tight both let the graph absorb all spare height (no legend by
// default), so the graph is the full interior; they differ only in the minimum
// height (kCompactMinH vs kTightMinH).
int compact_graph_h(int panel_h) { return panel_h - kBorderRows; }
int tight_graph_h(int panel_h) { return panel_h - kBorderRows; }

int graph_w(const PanelSpec& p, int panel_w, bool abbreviated) {
  const int axis = abbreviated ? abbrev_axis_width(p.y_scale.kind)
                               : axis_label_width(p.y_scale.kind);
  return panel_w - axis - 2 - kBorderRows;
}

bool best_mode(const PanelSpec& p, int w, int h, PanelMode& mode) {
  const int gw = graph_w(p, w, uses_abbrev_labels(w - kBorderRows));
  if (h >= full_height(p, w) && gw >= kFullCompactGraphW) {
    mode = PanelMode::kFull;
    return true;
  }
  if (h >= compact_height() && gw >= kFullCompactGraphW) {
    mode = PanelMode::kCompact;
    return true;
  }
  if (h >= tight_height() && gw >= kTightGraphW) {
    mode = PanelMode::kTight;
    return true;
  }
  return false;
}

// ---- the entry point --------------------------------------------------------

Layout compute_layout(int term_w, int term_h, std::span<const PanelSpec> panels,
                      const LayoutPrefs& prefs) {
  const int n = static_cast<int>(panels.size());
  Layout empty;
  if (n == 0 || term_w < 2 || term_h < kHeaderRows + kFooterRows) return empty;

  const int content_h = term_h - kHeaderRows - kFooterRows;

  int c_max = 1, c_lo = 1, c_hi = 1;
  column_range(n, term_w, panels, prefs, c_max, c_lo, c_hi);

  // Score every feasible column count, in two tiers: an ALL-FULL layout
  // always beats a degraded one (v1.0.1's standing preference — a Full panel
  // carries the legend and info rows, which no shrinking mode replaces), and
  // within a tier the candidates rank by better() (smallest graph height,
  // total graph area, degraded count). Ascending c + replace-on-strict-win
  // means ties keep the FEWER columns, per spec.
  Candidate best_full, best_mixed;
  for (int c = c_lo; c <= c_hi; ++c) {
    const Candidate cand = build_candidate(term_w, content_h, c, panels);
    if (!cand.valid) continue;
    if (cand.all_full) {
      if (!best_full.valid || better(cand, best_full)) best_full = cand;
    } else if (!best_mixed.valid || better(cand, best_mixed)) {
      best_mixed = cand;
    }
  }

  if (best_full.valid) return std::move(best_full.layout);
  if (best_mixed.valid) return std::move(best_mixed.layout);
  // Nothing fits even Tight at any column count: scroll, at the column count
  // that gives the fewest rows (c_max).
  return build_scroll(term_w, content_h, c_max, panels);
}

// ---- layout report (Part C) -------------------------------------------------

std::vector<CandidateReport> score_layout_candidates(int term_w, int term_h,
                                                     std::span<const PanelSpec> panels,
                                                     const LayoutPrefs& prefs) {
  const int n = static_cast<int>(panels.size());
  std::vector<CandidateReport> out;
  if (n == 0 || term_w < 2 || term_h < kHeaderRows + kFooterRows) return out;
  const int content_h = term_h - kHeaderRows - kFooterRows;
  int c_max = 1, c_lo = 1, c_hi = 1;
  column_range(n, term_w, panels, prefs, c_max, c_lo, c_hi);
  for (int c = c_lo; c <= c_hi; ++c) {
    const Candidate cand = build_candidate(term_w, content_h, c, panels);
    CandidateReport r;
    r.columns = c;
    r.valid = cand.valid;
    r.all_full = cand.all_full;
    r.degraded = cand.degraded;
    r.min_graph_h = cand.min_graph_h;
    r.total_graph_area = cand.total_graph_area;
    out.push_back(std::move(r));
  }
  return out;
}

int max_top_row(const Layout& lo, int content_h) {
  (void)content_h;  // the window model makes this offset-independent
  if (!lo.scrolling) return 0;
  return std::max(0, lo.total_rows - lo.rows());  // k = rows() visible rows
}

int rows_fitting(const Layout& lo, int content_h) {
  (void)content_h;
  return std::max(1, lo.rows());  // the window size (k when scrolling)
}

}  // namespace geth::ui
