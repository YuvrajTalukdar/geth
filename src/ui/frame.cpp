#include "ui/frame.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "ftxui/screen/string.hpp"  // string_width (wcwidth)

#include "util/format.hpp"
#include "util/names.hpp"

namespace geth::ui {

// ---------------------------------------------------------------------------
// braille dot encoding (exported for the unit tests)
// ---------------------------------------------------------------------------

int braille_bit(int col, int row) {
  // (col 0, rows 0..3 top->bottom) -> bits 0,1,2,6; (col 1) -> bits 3,4,5,7.
  // All eight dots set mask 0xFF, so a full cell is U+28FF.
  static const int kBits[2][4] = {{0, 1, 2, 6}, {3, 4, 5, 7}};
  return kBits[col][row];
}

std::string braille_char(std::uint8_t mask) {
  return std::string({'\xE2', static_cast<char>(0xA0 | (mask >> 6)),
                      static_cast<char>(0x80 | (mask & 0x3F))});
}

// ---------------------------------------------------------------------------
// y-axis scales (exported for the unit tests)
// ---------------------------------------------------------------------------

// Round up to a "nice" number: 1 / 2 / 2.5 / 5 x 10^n.
double nice_watts(double v) {
  if (v <= 0) return 0;
  const double p = std::pow(10.0, std::floor(std::log10(v)));
  for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
    if (v <= m * p + 1e-9) return m * p;
  return 10.0 * p;
}

// Binary units are powers of two, so the nice steps are powers of two too:
// 15 MiB -> 16 MiB, 100 MiB -> 128 MiB, 64 KiB -> 64 KiB (NOT 15 MiB -> 1 GiB).
double nice_bytes(double v) {
  if (v <= 0) return 0;
  double k = 1;
  for (;;) {
    for (double m : {1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 128.0})
      if (v <= m * k + 1e-9) return m * k;
    k *= 1024.0;
  }
}

// Grow immediately; shrink only after 10 s below 50 % of the current scale.
double update_auto_scale(FrameState::Auto& a, double target, double visible,
                         TimePoint now) {
  if (a.max <= 0 || target > a.max) {
    a.max = target;
    a.below = false;
  } else if (visible < 0.5 * a.max) {
    if (!a.below) {
      a.below = true;
      a.below_since = now;
    } else if (now - a.below_since >= std::chrono::seconds(10))
      a.max = target;
  } else {
    a.below = false;
  }
  return a.max;
}

// The y-axis max for this frame, updating the sticky/hysteresis state.
double panel_y_max(const PanelSpec& p, double vm, FrameState& st, TimePoint now) {
  switch (p.y_scale.kind) {
    case ScaleSpec::Kind::kPercent: return 100.0;
    case ScaleSpec::Kind::kTemperature:
      if (vm > 100.0) st.temp_max = vm > 110.0 ? 120.0 : 110.0;
      return st.temp_max;
    case ScaleSpec::Kind::kAbsolute:
      return p.y_scale.absolute_max > 0 ? p.y_scale.absolute_max : 1.0;
    case ScaleSpec::Kind::kWatts:
      return update_auto_scale(st.auto_scale[p.id],
                               std::max(nice_watts(vm), p.y_scale.floor_), vm, now);
    case ScaleSpec::Kind::kBytesPerSec:
      return update_auto_scale(st.auto_scale[p.id],
                               std::max(nice_bytes(vm), p.y_scale.floor_), vm, now);
  }
  return 100.0;
}

// ---------------------------------------------------------------------------
// geometry self-check (exported for the unit tests)
// ---------------------------------------------------------------------------

GeometryReport check_geometry(const std::string& text, int want_cols) {
  GeometryReport r;
  std::string t = text;
  if (!t.empty() && t.back() == '\n') t.pop_back();  // trailing NL = no new line
  std::size_t start = 0;
  for (;;) {
    const std::size_t nl = t.find('\n', start);
    const std::string line =
        t.substr(start, nl == std::string::npos ? nl : nl - start);
    ++r.lines;
    // string_width is wcwidth-based: display columns, not bytes or code points.
    if (ftxui::string_width(line) != want_cols) r.bad_lines.push_back(r.lines);
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  return r;
}

namespace {

constexpr int64_t kWindowNs = 60LL * 1000000000LL;  // fixed 60 s graph window

// ---------------------------------------------------------------------------
// pixel / text primitives (bounds-checked; the screen is the hard boundary)
// ---------------------------------------------------------------------------

void put_px(ftxui::Screen& s, int x, int y, const std::string& utf8,
            const ftxui::Color& fg, bool bold = false) {
  if (x < 0 || y < 0 || x >= s.dimx() || y >= s.dimy() || utf8.empty()) return;
  ftxui::Pixel& p = s.PixelAt(x, y);
  p.character = utf8;
  p.foreground_color = fg;
  p.bold = bold;
  // background_color is deliberately left Color::Default (transparency).
}

// Column count of a UTF-8 string (every glyph GETH emits is one cell wide).
int utf8_cols(const std::string& s) {
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

// First `cols` code points of s.
std::string utf8_cut(const std::string& s, int cols) {
  int n = 0, i = 0;
  while (i < (int)s.size() && n < cols) {
    const unsigned char c = s[i];
    if (c < 0x80) i += 1;
    else if ((c >> 5) == 6) i += 2;
    else if ((c >> 4) == 14) i += 3;
    else if ((c >> 3) == 30) i += 4;
    else i += 1;
    ++n;
  }
  return s.substr(0, i);
}

// Truncate to `w` columns, adding "…" when something was cut.
std::string truncate(const std::string& s, int w) {
  if (w <= 0) return "";
  if (utf8_cols(s) <= w) return s;
  if (w == 1) return "…";
  return utf8_cut(s, w - 1) + "…";
}

void put_str(ftxui::Screen& s, int x, int y, const std::string& utf8,
             const ftxui::Color& fg, bool bold = false) {
  int i = 0;
  while (i < (int)utf8.size()) {
    const unsigned char c = utf8[i];
    int len;
    if (c < 0x80) len = 1;
    else if ((c >> 5) == 6) len = 2;
    else if ((c >> 4) == 14) len = 3;
    else if ((c >> 3) == 30) len = 4;
    else len = 1;
    put_px(s, x++, y, utf8.substr(i, len), fg, bold);
    i += len;
  }
}

// ---------------------------------------------------------------------------
// series windows: fixed 60 s window, newest sample at the right edge
// ---------------------------------------------------------------------------

// In-window sample points (oldest -> newest) as a zero-copy view.
struct RawWindow {
  const int64_t* t = nullptr;
  const float* v = nullptr;
  std::size_t n = 0;
  int64_t t_start = 0, t_end = 0;
  double span_ns = 0;
  bool valid() const { return t && v && n > 0 && t_start < t_end; }
};

// Fill `t`/`v` (and point `w` at them) with one series' in-window points
// from the live store.
void load_live(RawWindow& w, std::vector<int64_t>& t, std::vector<float>& v,
               const SeriesStore& store, const SeriesId& id) {
  w = RawWindow{};
  t.clear();
  v.clear();
  const RingBuffer<float>* vals = store.values(id);
  const RingBuffer<int64_t>* times = store.times();
  if (!vals || !times) return;
  const std::size_t n = times->size();
  if (n == 0 || vals->size() != n) return;
  const int64_t t_end = times->at(n - 1);
  const int64_t span = std::min<int64_t>(kWindowNs, t_end - times->at(0));
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

// One point per dot column (plus the ends): enough for line interpolation at
// any width without iterating every sample of the 60 s window.
struct Decimated {
  std::vector<int64_t> t;
  std::vector<float> v;
};

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

// The value of a series at time `t` (ns), or NaN when it falls in a gap.
// Linear interpolation between the decimated samples keeps the line
// continuous at any width.
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

// ---------------------------------------------------------------------------
// scales (nice_watts / nice_bytes / update_auto_scale / panel_y_max are the
// exported versions above the anonymous namespace)
// ---------------------------------------------------------------------------

double visible_max(const std::vector<RawWindow>& raws) {
  double m = 0;
  for (const RawWindow& w : raws)
    for (std::size_t i = 0; i < w.n; ++i)
      if (std::isfinite(w.v[i]) && w.v[i] > m) m = w.v[i];
  return m;
}

// Compact tick labels: no trailing zeros, a space before the unit, °C for
// temperature — "100 %", "75 %", "0 %", "100 °C", "32 GiB", "30 W", "5 MiB/s".
// The legend keeps the full-precision formatters (format::bytes/watts/...).
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

// ---------------------------------------------------------------------------
// braille graph (braille_bit / braille_char are the exported versions above)
// ---------------------------------------------------------------------------

struct Cell {
  std::uint8_t mask = 0;
  int series = -1;   // last series that wrote into this cell
  int prio = -1;     // winning dot's kind: 0 = fill, 1 = line (line wins)
  int top_row = 99;  // winning dot's row within its cell
  ftxui::Color color;
};

struct GraphSeries {
  const Decimated* d;
  ftxui::Color line;
  ftxui::Color fill;
  bool filled;
};

// Fill `cells` (Wg x Hg) from all series. Drawn in legend order; when two
// series share a cell the later one wins. Within one series the topmost dot
// sets the cell color, and the LINE dot (prio 1) beats its own fill dot
// (prio 0) when they land in the same cell — the line is always drawn in the
// full series color, the fill in the derived fill color.
void draw_graph_cells(std::vector<Cell>& cells, int Wg, int Hg,
                      const std::vector<GraphSeries>& gs,
                      const std::vector<RawWindow>& raws, double vmax) {
  const int dot_cols = 2 * Wg;
  const int dot_rows = 4 * Hg;
  if (dot_cols < 2 || dot_rows < 4 || vmax <= 0) return;

  auto set_dot = [&](int xc, int yd, int sidx, const ftxui::Color& c, int prio) {
    Cell& cell = cells[static_cast<std::size_t>(yd / 4) * Wg + (xc / 2)];
    const bool later = cell.series < 0 || sidx > cell.series;
    const bool higher = sidx == cell.series &&
                        (prio > cell.prio ||
                         (prio == cell.prio && yd % 4 < cell.top_row));
    if (later || higher) {
      cell.color = c;
      cell.prio = prio;
      cell.top_row = yd % 4;
    }
    cell.series = std::max(cell.series, sidx);
    cell.mask = static_cast<std::uint8_t>(
        cell.mask | (1u << braille_bit(xc % 2, yd % 4)));
  };

  for (std::size_t si = 0; si < gs.size(); ++si) {
    const GraphSeries& g = gs[si];
    if (!g.d || g.d->t.empty() || !raws[si].valid()) continue;
    const RawWindow& w = raws[si];
    for (int xc = 0; xc < dot_cols; ++xc) {
      // The time under this dot column: real history over the 60 s window,
      // newest at the right edge.
      const double t =
          w.t_start + w.span_ns * (dot_cols > 1 ? xc / (double)(dot_cols - 1) : 1.0);
      const double v = value_at(*g.d, t);
      if (!std::isfinite(v)) continue;  // gap: never drawn as 0
      const double clamped = std::clamp(v, 0.0, vmax);
      int y = static_cast<int>(std::lround((1.0 - clamped / vmax) * (dot_rows - 1)));
      y = std::clamp(y, 0, dot_rows - 1);
      if (g.filled)
        for (int y2 = y; y2 < dot_rows; ++y2) set_dot(xc, y2, (int)si, g.fill, 0);
      set_dot(xc, y, (int)si, g.line, 1);
    }
  }
}

// ---------------------------------------------------------------------------
// colors / header text
// ---------------------------------------------------------------------------

struct Colors {
  ftxui::Color accent, title, text, label_dim, border, grid, axis_text, warn;
  ftxui::Color series[6];
  ftxui::Color fill[6];
};

Colors build_colors(const Theme& th, ColorMode m) {
  Colors c;
  c.accent = to_ftxui(th.accent, m);
  c.title = to_ftxui(th.title, m);
  c.text = to_ftxui(th.text, m);
  c.label_dim = to_ftxui(th.label_dim, m);
  c.border = to_ftxui(th.border, m);
  c.grid = to_ftxui(th.grid, m);
  c.axis_text = to_ftxui(th.axis_text, m);
  c.warn = to_ftxui(th.warn, m);
  for (int i = 0; i < 6; ++i) {
    c.series[i] = to_ftxui(th.series[i], m);
    c.fill[i] = to_ftxui(th.fill(i), m);
  }
  return c;
}

std::string interval_str(long long ms) {
  if (ms % 1000 == 0) return std::to_string(ms / 1000) + "s";
  return std::to_string(ms) + "ms";
}

std::string time_str() {
  std::time_t t = std::time(nullptr);
  std::tm tm{};
  localtime_r(&t, &tm);
  char b[16];
  strftime(b, sizeof b, "%H:%M:%S", &tm);
  return b;
}

// Does series `id` belong to a sleeping GPU?
bool series_sleeping(const Snapshot& snap, const SeriesId& id) {
  if (id.rfind("gpu.", 0) != 0) return false;
  static const char* const kSuffixes[] = {"usage", "vram", "gtt", "core_clock",
                                          "mem_clock", "temp", "power"};
  for (const char* suf : kSuffixes) {
    const std::size_t slen = std::strlen(suf);
    if (id.size() <= 4 + 1 + slen ||
        id.compare(id.size() - 1 - slen, 1 + slen, std::string(".") + suf) != 0)
      continue;
    const std::string bus = id.substr(4, id.size() - 5 - slen);
    for (const auto& g : snap.gpus)
      if (g.pci_bus_id == bus) return g.state == GpuState::kSleeping;
    return false;
  }
  return false;
}

// ---------------------------------------------------------------------------
// panel
// ---------------------------------------------------------------------------

// One value row: [▌label ... value] (bar or blank column, label truncated
// with "…" — never the value — value right-aligned in the slot). The label
// starts directly after "▌" (no space); bar-less rows align to the same x.
void draw_value_row(ftxui::Screen& s, int x, int y, int slot_w, bool has_bar,
                    const ftxui::Color& bar_c, const std::string& label,
                    const std::string& value, const ftxui::Color& text_c,
                    const ftxui::Color& accent_c) {
  if (slot_w < 1) return;
  const int vlen = utf8_cols(value);
  int vstart = x + slot_w - vlen;
  if (vstart < x + 1) vstart = x + 1;
  const int lx = x + 1;  // "▌label" or the aligned " label"
  int lmax = vstart - lx - 1;  // one gap column before the value
  if (lmax < 0) lmax = 0;
  if (has_bar) put_px(s, x, y, "▌", bar_c);
  put_str(s, lx, y, truncate(label, lmax), text_c);
  put_str(s, vstart, y, value, accent_c);
}

void draw_panel(ftxui::Screen& s, int x0, int y0, int W, int H, const PanelSpec& p,
                bool sleeping, const std::vector<RawWindow>& raws,
                const std::vector<std::string>& legend_values,
                const Snapshot& snap, double vmax, const Colors& c) {
  if (W < 4 || H < 2) return;

  // --- rounded border + title --------------------------------------------------
  for (int x = 1; x + 1 < W; ++x) {
    put_px(s, x0 + x, y0, "─", c.border);
    put_px(s, x0 + x, y0 + H - 1, "─", c.border);
  }
  put_px(s, x0, y0, "╭", c.border);
  put_px(s, x0 + W - 1, y0, "╮", c.border);
  put_px(s, x0, y0 + H - 1, "╰", c.border);
  put_px(s, x0 + W - 1, y0 + H - 1, "╯", c.border);
  for (int y = 1; y + 1 < H; ++y) {
    put_px(s, x0, y0 + y, "│", c.border);
    put_px(s, x0 + W - 1, y0 + y, "│", c.border);
  }

  std::string title = truncate(p.title, std::max(0, W - 8));
  std::string suffix = sleeping ? " · sleeping" : "";
  if (!suffix.empty()) {
    const int room = std::max(0, W - 8 - utf8_cols(suffix));
    title = truncate(p.title, room);
    if (title.empty()) suffix = "";  // keep the border clean
  }
  if (W >= 8 && !title.empty()) {
    put_px(s, x0 + 2, y0, " ", c.border);
    put_str(s, x0 + 3, y0, title, c.title);
    if (!suffix.empty())
      put_str(s, x0 + 3 + utf8_cols(title), y0, suffix, c.label_dim);
    put_px(s, x0 + 3 + utf8_cols(title) + utf8_cols(suffix), y0, " ", c.border);
  }

  // --- interior geometry --------------------------------------------------------
  const int ix0 = x0 + 1, iy0 = y0 + 1, iw = W - 2, ih = H - 2;
  if (iw < 3 || ih < 1) return;

  int n_legend = 0;
  std::vector<int> legend_idx;  // series indices that appear in the legend
  for (std::size_t i = 0; i < p.series.size(); ++i)
    if (p.series[i].in_legend) {
      legend_idx.push_back((int)i);
      ++n_legend;
    }
  const int ipr = (iw >= 56) ? 2 : 1;
  const int legend_rows = (n_legend + ipr - 1) / ipr;
  const int info_n = static_cast<int>(p.info_rows.size());
  int Hg = ih - legend_rows - info_n;
  if (Hg < 3) Hg = 0;  // not enough room: legend only, never overflow

  // Tick fractions (top -> bottom): 0/25/50/75/100, or 3 when short.
  std::vector<double> fracs;
  if (Hg >= 6)
    fracs = {1.0, 0.75, 0.5, 0.25, 0.0};
  else if (Hg >= 3)
    fracs = {1.0, 0.5, 0.0};

  int L = 0;
  std::vector<std::string> ticks;
  for (double f : fracs) {
    const std::string lab = tick_label(p.y_scale.kind, f, vmax);
    ticks.push_back(lab);
    L = std::max(L, utf8_cols(lab));
  }
  const bool have_graph = Hg >= 3 && iw - L - 2 >= 2 && vmax > 0;
  const int Wg = iw - L - 2;  // label column + " ┤"
  const int gx = ix0 + L + 2;
  const int gy = iy0;

  // --- braille graph + grid -------------------------------------------------------
  std::vector<Cell> cells;
  if (have_graph) {
    const int dot_cols = 2 * Wg;
    std::vector<Decimated> decs(p.series.size());
    std::vector<GraphSeries> gs(p.series.size());
    for (std::size_t i = 0; i < p.series.size(); ++i) {
      decs[i] = decimate(raws[i], dot_cols);
      const int ci = p.series[i].color_index;
      gs[i] = GraphSeries{&decs[i], c.series[ci], c.fill[ci], p.series[i].fill};
    }
    cells.resize(static_cast<std::size_t>(Wg) * Hg);
    draw_graph_cells(cells, Wg, Hg, gs, raws, vmax);
    for (int cy = 0; cy < Hg; ++cy)
      for (int cc = 0; cc < Wg; ++cc) {
        const Cell& cell = cells[static_cast<std::size_t>(cy) * Wg + cc];
        if (cell.mask) put_px(s, gx + cc, gy + cy, braille_char(cell.mask), cell.color);
      }
    // Gridlines: dim dotted "┈" at each tick row, only in cells with no data.
    for (std::size_t i = 0; i < fracs.size(); ++i) {
      const int ry = static_cast<int>(std::lround((1.0 - fracs[i]) * (Hg - 1)));
      for (int cc = 0; cc < Wg; ++cc)
        if (!cells[static_cast<std::size_t>(ry) * Wg + cc].mask)
          put_px(s, gx + cc, gy + ry, "┈", c.grid);
    }
  }

  // --- y axis: the data area starts at gx on EVERY graph row; the separator
  //     column is grid-color "│", tick rows carry the right-aligned label and
  //     a border-color "┤" ("label ┤"), non-tick rows a blank label.
  if (have_graph) {
    for (int ry = 0; ry < Hg; ++ry) put_px(s, ix0 + L + 1, iy0 + ry, "│", c.grid);
    for (std::size_t i = 0; i < fracs.size(); ++i) {
      const int ry = static_cast<int>(std::lround((1.0 - fracs[i]) * (Hg - 1)));
      const std::string& lab = ticks[i];
      put_str(s, ix0 + (L - utf8_cols(lab)), iy0 + ry, lab, c.axis_text);
      put_px(s, ix0 + L + 1, iy0 + ry, "┤", c.border);
    }
  }

  // --- legend rows (below the graph) ---------------------------------------------
  // Two columns: each value is right-aligned inside its own half, and at
  // least two blank columns separate the halves, so a value can never run
  // into the other column's "▌" ("12.00 GiB▌ Swap" is impossible).
  {
    const int gutter = 2;
    const int w0 = (ipr == 2) ? (iw - gutter) / 2 : iw;
    const int w1 = (ipr == 2) ? iw - gutter - w0 : iw;
    int y = iy0 + Hg;
    for (int k = 0; k < (int)legend_idx.size(); ++k) {
      const int si = legend_idx[k];
      const int col = k % ipr;
      const int sx = ix0 + (ipr == 2 ? col * (w0 + gutter) : 0);
      const int sw = (ipr == 2) ? (col == 0 ? w0 : w1) : iw;
      draw_value_row(s, sx, y + k / ipr, sw, true,
                     c.series[p.series[si].color_index], p.series[si].label,
                     legend_values[si], c.text, c.accent);
    }
  }

  // --- info rows -------------------------------------------------------------------
  {
    int y = iy0 + Hg + legend_rows;
    for (std::size_t i = 0; i < p.info_rows.size(); ++i) {
      const InfoRow& r = p.info_rows[i];
      const std::string value = r.value_fn ? r.value_fn(snap) : "—";
      draw_value_row(s, ix0, y + (int)i, iw, r.color_index >= 0,
                     r.color_index >= 0 ? c.series[r.color_index]
                                        : ftxui::Color::Default,
                     r.label, value, c.text, c.accent);
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------
// frame
// ---------------------------------------------------------------------------

void render_frame(ftxui::Screen& screen, const Hardware& hw,
                  const std::vector<PanelSpec>& page, const SeriesStore& store,
                  const Snapshot& latest, const FrozenFrame* frozen,
                  FrameState& state, const Theme& theme, ColorMode mode,
                  bool paused, long long interval_ms) {
  screen.Clear();
  const int W = screen.dimx(), H = screen.dimy();
  const Colors c = build_colors(theme, mode);
  const Snapshot& snap = frozen ? frozen->snap : latest;

  // Small terminals: never crash, show a centered notice below 40x12.
  if (W < 40 || H < 12) {
    const std::string shown = truncate("GETH — terminal too small (need 40×12)", W);
    put_str(screen, (W - utf8_cols(shown)) / 2, H / 2, shown, c.text);
    return;
  }

  // --- header: <CPU> & <primary dGPU>  [PAUSED] <interval> <time> -------------
  std::string htitle = names::short_cpu(hw.cpu_name);
  for (const auto& g : hw.gpus) {
    if (!g.is_integrated) {
      htitle += " & " + names::short_gpu(g.name, false, g.product_name);
      break;  // primary dGPU only
    }
  }
  const std::string rtime = time_str();
  const std::string rmid = interval_str(interval_ms) + " ";
  const std::string rpaused = paused ? "PAUSED " : "";
  {
    int x = W - utf8_cols(rtime);
    put_str(screen, x, 0, rtime, c.label_dim);
    x -= utf8_cols(rmid);
    put_str(screen, x, 0, rmid, c.label_dim);
    if (!rpaused.empty()) {
      x -= utf8_cols(rpaused);
      put_str(screen, x, 0, rpaused, c.warn);
    }
    put_str(screen, 0, 0, truncate(htitle, x - 1), c.title, true);
  }

  // --- footer ---------------------------------------------------------------------
  put_str(screen, 0, H - 1, truncate("q quit · space pause", W), c.label_dim);

  // --- panel grid: 2 equal-width columns, equal row heights -----------------------
  const int content_h = H - 2;
  const int n_rows = (int)((page.size() + 1) / 2);
  if (n_rows == 0) return;
  const int row_base = content_h / n_rows;
  const int row_extra = content_h % n_rows;
  const int left_w = W / 2;
  const int right_w = W - left_w;
  const TimePoint now = Clock::now();

  auto render_one = [&](int pi, int px, int py, int pw, int ph) {
    const PanelSpec& p = page[pi];
    std::vector<std::vector<int64_t>> ts(p.series.size());
    std::vector<std::vector<float>> vs(p.series.size());
    std::vector<RawWindow> raws(p.series.size());
    if (frozen) {
      for (std::size_t i = 0; i < p.series.size(); ++i)
        if (const FrozenFrame::Window* w = frozen->find(p.series[i].id))
          raws[i] = RawWindow{w->t.data(), w->v.data(), w->t.size(), w->t_start,
                              w->t_end, w->span_ns};
    } else {
      for (std::size_t i = 0; i < p.series.size(); ++i)
        load_live(raws[i], ts[i], vs[i], store, p.series[i].id);
    }

    bool sleeping = false;
    if (!p.gpu_bus.empty())
      for (const auto& g : snap.gpus)
        if (g.pci_bus_id == p.gpu_bus) {
          sleeping = g.state == GpuState::kSleeping;
          break;
        }

    const double vm = visible_max(raws);
    const double vmax = panel_y_max(p, vm, state, now);

    std::vector<std::string> legend_values(p.series.size(), "—");
    for (std::size_t i = 0; i < p.series.size(); ++i)
      if (series_sleeping(snap, p.series[i].id))
        legend_values[i] = "sleeping";
      else if (p.series[i].value_fn)
        legend_values[i] = p.series[i].value_fn(snap);

    draw_panel(screen, px, py, pw, ph, p, sleeping, raws, legend_values, snap, vmax, c);
  };

  int y = 1;
  for (int r = 0; r < n_rows; ++r) {
    const int rh = row_base + (r < row_extra ? 1 : 0);
    const int pi0 = r * 2;
    if (pi0 < (int)page.size()) render_one(pi0, 0, y, left_w, rh);
    if (pi0 + 1 < (int)page.size()) render_one(pi0 + 1, left_w, y, right_w, rh);
    y += rh;
  }
}

namespace {

// The whole frame as one custom FTXUI node: Draw() hands us the screen, we
// fill it. Nothing else in the element tree can set a background.
class FrameNode : public ftxui::Node {
 public:
  explicit FrameNode(std::function<void(ftxui::Screen&)> draw)
      : _draw(std::move(draw)) {}
  void Render(ftxui::Screen& screen) override {
    screen.Clear();
    _draw(screen);
  }

 private:
  std::function<void(ftxui::Screen&)> _draw;
};

}  // namespace

ftxui::Element build_frame_element(const Hardware& hw,
                                   const std::vector<PanelSpec>& page,
                                   const SeriesStore& store, const Snapshot& latest,
                                   const FrozenFrame* frozen, FrameState& state,
                                   const Theme& theme, ColorMode mode, bool paused,
                                   long long interval_ms) {
  return std::make_shared<FrameNode>(
      [&hw, &page, &store, &latest, frozen, &state, &theme, mode, paused,
       interval_ms](ftxui::Screen& screen) {
        render_frame(screen, hw, page, store, latest, frozen, state, theme, mode,
                     paused, interval_ms);
      });
}

std::string screen_to_text(const ftxui::Screen& screen) {
  std::string out;
  out.reserve((std::size_t)screen.dimx() * screen.dimy() * 2);
  for (int y = 0; y < screen.dimy(); ++y) {
    for (int x = 0; x < screen.dimx(); ++x) {
      const std::string& ch = screen.at(x, y);
      out += ch.empty() ? " " : ch;
    }
    if (y + 1 < screen.dimy()) out += '\n';
  }
  return out;
}

void FrameStats::print() const {
  std::printf("geth stats: %llu frames, %llu samples\n", frames, samples);
  std::printf("  render time   mean %9.1f us  max %10.1f us\n", render_us_mean(),
              render_us_max);
  std::printf("  sample time   mean %9.1f us  max %10.1f us\n", sample_us_mean(),
              sample_us_max);
  std::printf("  allocs/frame  mean %9.1f    max %10lld\n", allocs_mean(), allocs_max);
}

FrozenFrame capture_frozen(const std::vector<PanelSpec>& page,
                           const SeriesStore& store, const Snapshot& latest) {
  FrozenFrame f;
  f.snap = latest;
  for (const auto& p : page)
    for (const auto& sr : p.series) {
      if (f.find(sr.id)) continue;
      FrozenFrame::Window w;
      w.id = sr.id;
      RawWindow raw;
      load_live(raw, w.t, w.v, store, sr.id);
      if (!raw.valid()) continue;
      w.t_start = raw.t_start;
      w.t_end = raw.t_end;
      w.span_ns = raw.span_ns;
      f.windows.push_back(std::move(w));
    }
  return f;
}

}  // namespace geth::ui
