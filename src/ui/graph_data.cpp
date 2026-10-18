#include "ui/graph_data.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "util/format.hpp"

namespace geth::ui {

// Fill `t`/`v` (and point `w` at them) with one series' in-window points
// from the live store. The window is the newest `window_ns` of history
// (the graph time window: 30 s .. 10 m, cycled with t); the ring holds a
// full hour, so every window is a slice of it.
void load_live(RawWindow& w, std::vector<int64_t>& t, std::vector<float>& v,
               const SeriesStore& store, const SeriesId& id, int64_t window_ns) {
  w = RawWindow{};
  t.clear();
  v.clear();
  const RingBuffer<float>* vals = store.values(id);
  const RingBuffer<int64_t>* times = store.times();
  if (!vals || !times) return;
  const std::size_t n = times->size();
  if (n == 0 || vals->size() != n) return;
  const int64_t t_end = times->at(n - 1);
  const int64_t span = std::min<int64_t>(window_ns, t_end - times->at(0));
  if (span <= 0) return;
  const int64_t t_start = t_end - span;
  t.reserve(n);
  v.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const int64_t ti = times->at(i);
    if (ti < t_start) continue;
    t.push_back(ti);
    v.push_back(vals->at(i));
  }
  w.t = t.data();
  w.v = v.data();
  w.n = t.size();
  w.t_start = t_start;
  w.t_end = t_end;
  w.span_ns = static_cast<double>(span);
}

Decimated decimate(const RawWindow& w, int dot_cols) {
  Decimated d;
  if (!w.valid() || dot_cols < 1) return d;
  auto col = [&](int64_t ti) -> int {
    return static_cast<int>(static_cast<double>(ti - w.t_start) / w.span_ns *
                            (dot_cols - 1));
  };
  d.t.reserve(w.n);
  d.v.reserve(w.n);
  int prev = -1;
  for (std::size_t i = 0; i < w.n; ++i) {
    const int c = col(w.t[i]);
    if (i == w.n - 1 || c != prev) {
      d.t.push_back(w.t[i]);
      d.v.push_back(w.v[i]);
      prev = c;
    }
  }
  return d;
}

double value_at(const Decimated& d, double t) {
  const std::size_t n = d.t.size();
  if (n == 0) return std::nan("");
  if (n == 1) return std::abs(t - (double)d.t[0]) < 1e3 ? d.v[0] : std::nan("");
  std::size_t p = 0;
  while (p + 1 < n && t >= (double)d.t[p + 1]) ++p;
  if (p + 1 >= n)
    return std::abs(t - (double)d.t[n - 1]) < 1e3 ? d.v[n - 1] : std::nan("");
  const double t0 = d.t[p], t1 = d.t[p + 1];
  if (!std::isfinite(d.v[p]) || !std::isfinite(d.v[p + 1])) return std::nan("");
  if (t1 <= t0) return d.v[p + 1];
  const double f = (t - t0) / (t1 - t0);
  return d.v[p] + f * (d.v[p + 1] - d.v[p]);
}

double visible_max(const std::vector<RawWindow>& raws) {
  double m = 0;
  for (const RawWindow& w : raws)
    for (std::size_t i = 0; i < w.n; ++i)
      if (std::isfinite(w.v[i]) && w.v[i] > m) m = w.v[i];
  return m;
}

std::string tick_label(ScaleSpec::Kind k, double f, double vmax) {
  switch (k) {
    case ScaleSpec::Kind::kPercent: return format::compact_percent(f * vmax);
    case ScaleSpec::Kind::kTemperature: return format::compact_celsius(f * vmax);
    case ScaleSpec::Kind::kAbsolute: return format::compact_bytes(f * vmax);
    case ScaleSpec::Kind::kWatts: return format::compact_watts(f * vmax);
    case ScaleSpec::Kind::kBytesPerSec: return format::compact_rate(f * vmax);
  }
  return "0";
}

std::string abbrev_tick_label(ScaleSpec::Kind k, double f, double vmax) {
  switch (k) {
    case ScaleSpec::Kind::kPercent: return format::abbrev_percent(f * vmax);
    case ScaleSpec::Kind::kTemperature: return format::abbrev_celsius(f * vmax);
    case ScaleSpec::Kind::kAbsolute: return format::abbrev_bytes(f * vmax);
    case ScaleSpec::Kind::kWatts: return format::abbrev_watts(f * vmax);
    case ScaleSpec::Kind::kBytesPerSec: return format::abbrev_rate(f * vmax);
  }
  return "0";
}

GraphGeom compute_graph_geom(int x0, int y0, int W, int H, PanelMode mode,
                             const PanelSpec& p, bool title_truncated,
                             double vmax) {
  GraphGeom g;
  g.x0 = x0; g.y0 = y0; g.W = W; g.H = H;
  g.ix0 = x0 + 1; g.iy0 = y0 + 1; g.iw = W - 2; g.ih = H - 2;
  if (g.iw < 3 || g.ih < 1) return g;

  const bool full = (mode == PanelMode::kFull);
  const bool tight = (mode == PanelMode::kTight);

  int n_legend = 0;
  for (std::size_t i = 0; i < p.series.size(); ++i)
    if (p.series[i].in_legend && !p.series[i].hidden) {
      g.legend_idx.push_back((int)i);
      ++n_legend;
    }
  g.ipr = legend_cols(g.iw);  // 3 at >=96, 2 at >=56, else 1
  // Full: legend rows + info rows, the graph takes what is left. Compact: the
  // graph takes the WHOLE interior (spare height goes to the graph); a
  // one-line legend is drawn only when the border title truncated with "…"
  // and a spare row exists, so the values are not lost. Tight: whole interior
  // for the graph, no legend at all.
  g.legend_rows = full ? (n_legend + g.ipr - 1) / g.ipr : 0;
  g.info_n = full ? static_cast<int>(p.info_rows.size()) : 0;
  g.Hg = g.ih - g.legend_rows - g.info_n;
  if (!full && !tight && title_truncated && n_legend > 0 && g.Hg > 3) {
    ++g.legend_rows;  // spend one spare row on the legend (graph min is 3)
    --g.Hg;
  }
  // Minimum graph height per mode (the layout engine guarantees it):
  // Full 4, Compact 3, Tight 2.
  const int hg_min = full ? 4 : (tight ? 2 : 3);
  if (g.Hg < hg_min) g.Hg = 0;  // not enough room: legend only, never overflow

  // Tick fractions (top -> bottom): Tight shows only the top and bottom
  // label; Full/Compact show 0/25/50/75/100, or 3 when short.
  if (tight)
    g.fracs = {1.0, 0.0};
  else if (g.Hg >= 6)
    g.fracs = {1.0, 0.75, 0.5, 0.25, 0.0};
  else if (g.Hg >= 3)
    g.fracs = {1.0, 0.5, 0.0};

  // Narrow panels (inner width < 60) use the abbreviated axis form in EVERY
  // mode — the same rule the layout engine sizes the axis with
  // (layout.hpp: uses_abbrev_labels), so the two can never disagree.
  const bool abbrev = uses_abbrev_labels(g.iw);
  for (double f : g.fracs) {
    const std::string lab =
        abbrev ? abbrev_tick_label(p.y_scale.kind, f, vmax)
               : tick_label(p.y_scale.kind, f, vmax);
    g.ticks.push_back(lab);
    g.label_w = std::max(g.label_w, utf8_cols(lab));
  }

  g.vmax = vmax;
  g.have_graph = g.Hg >= hg_min && g.iw - g.label_w - 2 >= 2 && vmax > 0;
  g.Wg = g.iw - g.label_w - 2;  // label column + " ┤"
  g.gx = g.ix0 + g.label_w + 2;
  g.gy = g.iy0;
  g.dot_cols = 2 * g.Wg;
  return g;
}

}  // namespace geth::ui
