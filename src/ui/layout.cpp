#include "ui/layout.hpp"

#include <algorithm>

namespace geth::ui {

namespace {

constexpr int kHeaderRows = 1;        // the title line
constexpr int kFooterRows = 1;        // the key-hints line
constexpr int kBorderRows = 2;        // top + bottom border of a panel
constexpr int kMinGraphW = 32;        // minimum graph width, both modes
constexpr int kFullGraphMin = 4;      // Full mode: the graph needs >=4 rows
constexpr int kCompactGraphMin = 3;   // Compact mode: the graph needs >=3 rows

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
  bool valid = false;      // every panel fits (Full or Compact)
  bool all_full = false;   // and every one of them in Full mode
  int min_graph_h = 0;     // smallest graph height across the panels
  long long total_graph_area = 0;  // sum of graph_h x graph_w
  Layout layout;
};

// True when `a` beats `b`: more Full panels first, then a taller smallest
// graph, then more total graph area. Ties keep the earlier (fewer-column)
// candidate because the caller only replaces on a strict win.
bool better(const Candidate& a, const Candidate& b) {
  if (a.all_full != b.all_full) return a.all_full;
  if (a.min_graph_h != b.min_graph_h) return a.min_graph_h > b.min_graph_h;
  return a.total_graph_area > b.total_graph_area;
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
  int page_h = 0;
  for (int r : lo.row_heights) page_h += r;
  lo.page_height = page_h;

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

      // Mode: Full if it fits, else Compact if it fits, else it does not.
      if (w >= min_panel_width(p) && h >= full_height(p, w)) {
        pl.mode = PanelMode::kFull;
      } else if (w >= min_panel_width(p) && h >= compact_height()) {
        pl.mode = PanelMode::kCompact;
        all_full = false;
      } else {
        all_fit = false;
        pl.mode = PanelMode::kCompact;  // placeholder; the candidate is invalid
      }
      if (all_fit) {
        const int gh = (pl.mode == PanelMode::kFull) ? full_graph_h(p, w, h)
                                                     : compact_graph_h(h);
        const int gw = graph_w(p, w);
        if (min_h == 0 || gh < min_h) min_h = gh;
        area += static_cast<long long>(gh) * gw;
      }
    }
    y += lo.row_heights[r];
  }
  cand.valid = all_fit;
  cand.all_full = all_fit && all_full;
  cand.min_graph_h = min_h;
  cand.total_graph_area = area;
  return cand;
}

// The scroll fallback: c_max columns (the fewest rows, hence the shortest
// page), every row at the minimum Compact height. The page is taller than the
// content area by construction — that is why we are scrolling.
Layout build_scroll(int term_w, int content_h, int c,
                    std::span<const PanelSpec> panels) {
  Layout lo;
  const int n = static_cast<int>(panels.size());
  const int rows = (n + c - 1) / c;
  const int rh = compact_height();
  lo.columns = c;
  lo.content_height = content_h;
  lo.row_heights.assign(rows, rh);
  lo.page_height = rows * rh;
  lo.scrolling = true;
  lo.panels.assign(n, {});

  const std::vector<int> col_w = split_even(term_w, c);
  std::vector<int> col_x(c, 0);
  for (int j = 1; j < c; ++j) col_x[j] = col_x[j - 1] + col_w[j - 1];
  const int last_k = n - (rows - 1) * c;
  const bool last_partial = last_k < c;
  std::vector<int> last_w, last_x;
  if (last_partial) {
    last_w = split_even(term_w, last_k);
    last_x.assign(last_k, 0);
    for (int j = 1; j < last_k; ++j) last_x[j] = last_x[j - 1] + last_w[j - 1];
  }

  int y = 0;
  for (int r = 0; r < rows; ++r) {
    const bool is_last = (r == rows - 1);
    const int k_in_row = is_last ? last_k : c;
    for (int m = 0; m < k_in_row; ++m) {
      const int i = r * c + m;
      if (i >= n) break;
      const int w = (is_last && last_partial) ? last_w[m] : col_w[m];
      const int x = (is_last && last_partial) ? last_x[m] : col_x[m];
      lo.panels[i].rect = LayoutRect{x, y, w, rh, r};
      lo.panels[i].mode = PanelMode::kCompact;
    }
    y += rh;
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
  int need = 0;
  for (const auto& p : panels) need = std::max(need, min_panel_width(p));
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

int min_panel_width(const PanelSpec& p) {
  return kMinGraphW + axis_label_width(p.y_scale.kind) + 2 + kBorderRows;
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

int compact_height() { return kBorderRows + kCompactGraphMin + 1; }

int full_graph_h(const PanelSpec& p, int panel_w, int panel_h) {
  const int iw = panel_w - kBorderRows;
  const int ih = panel_h - kBorderRows;
  return ih - legend_rows(p, iw) - static_cast<int>(p.info_rows.size());
}

int compact_graph_h(int panel_h) { return panel_h - kBorderRows - 1; }

int graph_w(const PanelSpec& p, int panel_w) {
  return panel_w - axis_label_width(p.y_scale.kind) - 2 - kBorderRows;
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

  // Score every feasible column count. Ascending c + replace-on-strict-win
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
  // Nothing fits even Compact: scroll, at the column count that gives the
  // shortest page (c_max -> the fewest rows).
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
    r.min_graph_h = cand.min_graph_h;
    r.total_graph_area = cand.total_graph_area;
    out.push_back(std::move(r));
  }
  return out;
}

int max_top_row(const Layout& lo, int content_h) {
  if (!lo.scrolling) return 0;
  const int need = lo.page_height - content_h;  // >= 1 while scrolling
  int acc = 0;
  for (int r = 0; r < lo.rows(); ++r) {
    if (acc >= need) return r;
    acc += lo.row_heights[r];
  }
  return lo.rows() - 1;
}

int rows_fitting(const Layout& lo, int content_h) {
  int n = 0, acc = 0;
  for (int r = 0; r < lo.rows(); ++r) {
    if (acc + lo.row_heights[r] > content_h) break;
    acc += lo.row_heights[r];
    ++n;
  }
  return std::max(n, 1);
}

}  // namespace geth::ui
