#include "ui/frame.hpp"
#include "ui/graph_data.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "ftxui/screen/string.hpp"  // string_width (wcwidth)

#include "util/format.hpp"
#include "util/names.hpp"
#include "util/trace.hpp"

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

// Round up so the ceiling's QUARTER is a nice number {1, 2, 2.5, 5} x 10^n:
// the ceiling itself is then {4, 8, 10, 20} x 10^n (20, 40, 80, 100, 200,
// 400 W, ...), and every tick label — 0/25/50/75/100 % of the ceiling — is a
// whole number of watts, so a "7.5 W" label is impossible. Minimum ceiling
// 20 W (its quarter is 5 W).
double nice_watts(double v) {
  const double lo = std::max(v, 20.0);
  for (double p = 1.0;; p *= 10.0)
    for (double m : {4.0, 8.0, 10.0, 20.0})
      if (lo <= m * p + 1e-9) return m * p;
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

// Absolute scales (RAM/VRAM totals): the smallest value >= v from the
// sequence {1, 1.5} x 2^k. 1024^n is a power of two, so the ceiling in raw
// bytes equals the ceiling in whatever display unit compact_bytes would pick:
// 22.8 GiB -> 24 GiB, 15.4 GiB -> 16 GiB, 4 GiB -> 4 GiB, 512 MiB -> 512 MiB.
double nice_bytes_max(double v) {
  if (v <= 0) return 0;
  for (double k = 1.0;; k *= 2.0)
    for (double m : {1.0, 1.5}) {
      const double cand = m * k;
      if (v <= cand + cand * 1e-9) return cand;  // relative eps: v is in bytes
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
      // Round the total up to {1, 1.5} x 2^k so the top tick is a round
      // number in the display unit (22.8 GiB -> 24 GiB, ticks 0/6/12/18/24).
      return p.y_scale.absolute_max > 0 ? nice_bytes_max(p.y_scale.absolute_max)
                                        : 1.0;
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
// braille graph (braille_bit / braille_char are the exported versions above)
// ---------------------------------------------------------------------------

// One character cell of the graph rect. `mask` is the OR of the braille dots
// of every series that landed in it (several series may share a cell);
// `color` belongs to the LAST writer (draw order: series 1..n-1, then the
// primary). In the area chart, `glyph` holds the exact glyph to draw (▁..█);
// in braille mode it stays empty. `order == -1` = no data in the cell.
struct Cell {
  std::uint8_t mask = 0;
  std::string glyph;  // area chart: exact glyph ("" = braille mask)
  int order = -1;     // draw order of the last writer (-1 = empty)
  ftxui::Color color;
};

struct GraphSeries {
  const Decimated* d;
  ftxui::Color line;
};

// Bresenham in dot space: plots every dot from (x0,y0) to (x1,y1) EXCEPT the
// start (already drawn as the previous column's line dot). Between two
// adjacent sample columns (x1 == x0 + 1) this makes the line continuous: a
// steep rise or fall fills an unbroken vertical run of dots instead of
// leaving isolated dots with a gap between them.
template <typename F>
void bresenham_dots(int x0, int y0, int x1, int y1, F plot) {
  const int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
  const int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  while (x0 != x1 || y0 != y1) {
    const int e2 = 2 * err;
    if (e2 >= dy && x0 != x1) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx && y0 != y1) {
      err += dx;
      y0 += sy;
    }
    plot(x0, y0);
  }
}

// Fill `cells` (Wg x Hg) with ONE-dot-thick braille lines, Bresenham-
// continuous in dot space (multi-series panels). When two series share a
// cell the one drawn LAST wins — series 1..n-1 are drawn in order and the
// PRIMARY (series 0, the first entry) is drawn last, so it sits on top in a
// shared cell. Text mode draws no fill: single-series panels use the area
// chart below, and the pixel renderer carries the fill otherwise.
void draw_graph_cells(std::vector<Cell>& cells, int Wg, int Hg,
                      const std::vector<GraphSeries>& gs,
                      const std::vector<RawWindow>& raws, double vmax) {
  const int dot_cols = 2 * Wg;
  const int dot_rows = 4 * Hg;
  if (dot_cols < 2 || dot_rows < 2 || vmax <= 0) return;

  auto set_dot = [&](int xc, int yd, int order, const ftxui::Color& c) {
    Cell& cell = cells[static_cast<std::size_t>(yd / 4) * Wg + (xc / 2)];
    if (order > cell.order) {
      cell.color = c;
      cell.order = order;
    }
    cell.mask = static_cast<std::uint8_t>(cell.mask | (1u << braille_bit(xc % 2, yd % 4)));
  };

  // Draw order: the primary (series 0) last so it wins shared cells.
  const int n = static_cast<int>(gs.size());
  for (int pass = 0; pass < 2; ++pass) {
    for (int si = 0; si < n; ++si) {
      // pass 0: series 1..n-1 (in legend order); pass 1: the primary (0).
      if (pass == 0 ? si == 0 : si != 0) continue;
      const GraphSeries& g = gs[si];
      if (!g.d || g.d->t.empty() || !raws[si].valid()) continue;
      const int order = (si == 0) ? n : si;  // n > any non-primary order
      const RawWindow& w = raws[si];
      bool have_prev = false;  // line restarts after a gap
      int prev_x = 0, prev_y = 0;  // last drawn line dot
      for (int xc = 0; xc < dot_cols; ++xc) {
        // The time under this dot column: real history over the 60 s window,
        // newest at the right edge.
        const double t =
            w.t_start + w.span_ns * (dot_cols > 1 ? xc / (double)(dot_cols - 1) : 1.0);
        const double v = value_at(*g.d, t);
        if (!std::isfinite(v)) {
          have_prev = false;  // gap: never drawn as 0, line does not span it
          continue;
        }
        const double clamped = std::clamp(v, 0.0, vmax);
        int y = static_cast<int>(std::lround((1.0 - clamped / vmax) * (dot_rows - 1)));
        y = std::clamp(y, 0, dot_rows - 1);
        // Continuous connection to the previous column (its start dot was
        // already drawn, so Bresenham plots only the new dots).
        if (have_prev && prev_x == xc - 1)
          bresenham_dots(prev_x, prev_y, xc, y,
                         [&](int bx, int by) { set_dot(bx, by, order, g.line); });
        set_dot(xc, y, order, g.line);
        have_prev = true;
        prev_x = xc;
        prev_y = y;
      }
    }
  }
}

// The area chart's top cell, per eighth of a cell: U+2581..U+2588 (▁..█);
// index 0 = no ink in the column.
static const char* const kEighths[9] = {
    " ", "\xE2\x96\x81", "\xE2\x96\x82", "\xE2\x96\x83", "\xE2\x96\x84",
    "\xE2\x96\x85", "\xE2\x96\x86", "\xE2\x96\x87", "\xE2\x96\x88"};

// The single-series panel: a solid AREA chart (the KDE look in text form).
// For every character column the series value (interpolated at the column's
// time) sets a fill height in EIGHTHS of a cell: the bottom h8/8 cells are
// full blocks, the next cell the matching eighth glyph. Each column carries
// its own vertical gradient — the bottom row is the series color scaled to
// 0.45, the column's top row the full color; fill_color keeps the shade in
// the series' hue family in 256/16 modes. NaN breaks the column (never a
// 0-floor).
void draw_area_cells(std::vector<Cell>& cells, int Wg, int Hg,
                     const Decimated& d, const RawWindow& w, double vmax,
                     const Rgb& series, ColorMode mode) {
  if (Wg < 1 || Hg < 1 || vmax <= 0) return;
  for (int cc = 0; cc < Wg; ++cc) {
    const double t =
        w.t_start + w.span_ns * (Wg > 1 ? cc / (double)(Wg - 1) : 1.0);
    const double v = value_at(d, t);
    if (!std::isfinite(v)) continue;  // gap: the column stays empty
    const double clamped = std::clamp(v, 0.0, vmax);
    int h8 = static_cast<int>(std::lround(clamped / vmax * Hg * 8.0));
    h8 = std::clamp(h8, 0, Hg * 8);
    const int full = h8 / 8;           // full cells, counted from the bottom
    const int eighth = h8 % 8;         // the partial cell above them
    const int top_b = full + (eighth > 0 ? 1 : 0) - 1;  // top row, bottom-index
    for (int b = 0; b <= top_b; ++b) {
      const int cy = Hg - 1 - b;  // character row (0 = top)
      Cell& cell = cells[static_cast<std::size_t>(cy) * Wg + cc];
      const double f = (top_b > 0) ? 0.45 + 0.55 * b / (double)top_b : 1.0;
      cell.glyph = (b == top_b && eighth > 0) ? kEighths[eighth] : "\xE2\x96\x88";
      cell.color = fill_color(series, mode, f);
      cell.order = 0;
      cell.mask = 0;
    }
  }
}

// ---------------------------------------------------------------------------
// colors / header text
// ---------------------------------------------------------------------------

struct Colors {
  ftxui::Color accent, title, text, label_dim, border, grid, axis_text, warn;
  ftxui::Color series[6];
  // Phase 5: the theme's background, painted only in solid mode.
  ftxui::Color bg;
  // The theme's raw series colors and the draw mode: the area chart's
  // per-column gradient scales these (fill_color keeps the hue family).
  Rgb series_raw[6];
  ColorMode mode;
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
  c.bg = to_ftxui(th.bg, m);
  // In 16-color mode the six series get an INJECTIVE slot assignment (no two
  // series share a slot); 256/truecolor use the plain nearest color.
  std::uint8_t slots16[6];
  if (m == ColorMode::Color16) series16_slots(th.series, slots16);
  c.mode = m;
  for (int i = 0; i < 6; ++i) {
    c.series[i] = (m == ColorMode::Color16)
                      ? ftxui::Color(ftxui::Color::Palette16(slots16[i]))
                      : to_ftxui(th.series[i], m);
    c.series_raw[i] = th.series[i];
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

// ---------------------------------------------------------------------------
// Compact-mode title + legend
// ---------------------------------------------------------------------------

// Short form of a series label for a Compact title: a power-source label
// ("APU Package (PPT)") keeps its first token ("APU"); a GPU name keeps its
// first two tokens ("RTX 3050 Laptop" -> "RTX 3050").
std::string short_title_label(const std::string& label) {
  if (label.find("Package") != std::string::npos) {
    const auto sp = label.find(' ');
    return sp == std::string::npos ? label : label.substr(0, sp);
  }
  int tokens = 0, i = 0;
  while (i < (int)label.size() && tokens < 2) {
    if (label[i] == ' ') ++tokens;
    ++i;
  }
  std::string t = label.substr(0, i);
  while (!t.empty() && t.back() == ' ') t.pop_back();
  return t;
}

// The Compact title: "<title> · <key1> · <key2> …" (the caller truncates it).
// Multi-series panels show "short label + value" per series; single-series
// panels (CPU, GPU) carry their key values in the info rows, which are appended
// here (duplicates of an earlier key are dropped).
std::string compact_title(const PanelSpec& p,
                          const std::vector<std::string>& legend_values,
                          const Snapshot& snap) {
  int n_leg = 0;
  for (const auto& s : p.series)
    if (s.in_legend && !s.hidden) ++n_leg;
  const bool multi = n_leg >= 2;
  std::string t = p.title;
  for (std::size_t i = 0; i < p.series.size(); ++i) {
    if (!p.series[i].in_legend || p.series[i].hidden) continue;
    const std::string v = legend_values[i];
    t += " \xC2\xB7 " + (multi ? short_title_label(p.series[i].label) + " " + v : v);
  }
  if (n_leg <= 1)
    for (const auto& r : p.info_rows) {
      if (!r.value_fn) continue;
      const std::string v = r.value_fn(snap);
      bool dup = false;
      for (std::size_t i = 0; i < p.series.size(); ++i)
        if (p.series[i].in_legend && !p.series[i].hidden &&
            legend_values[i] == v)
          dup = true;
      if (dup) continue;
      t += " \xC2\xB7 " + v;
    }
  return t;
}

// The Compact legend: every series on ONE line, "▌label value" entries
// separated by two columns, the tail truncated with "…" when it does not fit.
void draw_compact_legend(ftxui::Screen& s, int x, int y, int iw,
                         const PanelSpec& p,
                         const std::vector<std::string>& legend_values,
                         const Colors& c) {
  int cx = x;
  const int xend = x + iw;
  for (std::size_t i = 0; i < p.series.size(); ++i) {
    if (!p.series[i].in_legend || p.series[i].hidden) continue;
    const SeriesSpec& sr = p.series[i];
    const std::string& val = legend_values[i];
    const int entry = 1 + utf8_cols(sr.label) + 1 + utf8_cols(val);
    if (cx + entry > xend) {
      // Truncate the entry to the remaining room and stop.
      if (xend - cx >= 2) {
        put_px(s, cx, y, "▌", c.series[sr.color_index]);
        put_str(s, cx + 1, y, truncate(sr.label + " " + val, xend - cx - 1), c.text);
      }
      break;
    }
    put_px(s, cx, y, "▌", c.series[sr.color_index]);
    put_str(s, cx + 1, y, sr.label, c.text);
    put_str(s, cx + 1 + utf8_cols(sr.label) + 1, y, val, c.accent);
    cx += entry + 2;
  }
}

void draw_panel(ftxui::Screen& s, int x0, int y0, int W, int H, const PanelSpec& p,
                PanelMode mode, bool sleeping, const std::vector<RawWindow>& raws,
                const std::vector<std::string>& legend_values,
                const Snapshot& snap, double vmax, const Colors& c,
                bool pixels, bool focused = false) {
  if (W < 4 || H < 2) return;

  // --- rounded border + title --------------------------------------------------
  // The focused panel (Tab / click) gets the accent border.
  const ftxui::Color& bc = focused ? c.accent : c.border;
  for (int x = 1; x + 1 < W; ++x) {
    put_px(s, x0 + x, y0, "─", bc);
    put_px(s, x0 + x, y0 + H - 1, "─", bc);
  }
  put_px(s, x0, y0, "╭", bc);
  put_px(s, x0 + W - 1, y0, "╮", bc);
  put_px(s, x0, y0 + H - 1, "╰", bc);
  put_px(s, x0 + W - 1, y0 + H - 1, "╯", bc);
  for (int y = 1; y + 1 < H; ++y) {
    put_px(s, x0, y0 + y, "│", bc);
    put_px(s, x0 + W - 1, y0 + y, "│", bc);
  }

  // Full: the panel title. Compact/Tight: the title plus the key values (the
  // values live in the border, so no separate legend row by default).
  const bool full = (mode == PanelMode::kFull);
  std::string title = full ? p.title : compact_title(p, legend_values, snap);
  const std::string suffix = sleeping ? " \xC2\xB7 sleeping" : "";
  title = truncate(title, std::max(0, W - 8 - utf8_cols(suffix)));
  const bool title_truncated =
      title.size() >= 3 &&
      title.compare(title.size() - 3, 3, "\xE2\x80\xA6") == 0;  // ends in "…"
  if (W >= 8 && !title.empty()) {
    put_px(s, x0 + 2, y0, " ", c.border);
    put_str(s, x0 + 3, y0, title, c.title);
    if (!suffix.empty())
      put_str(s, x0 + 3 + utf8_cols(title), y0, suffix, c.label_dim);
    put_px(s, x0 + 3 + utf8_cols(title) + utf8_cols(suffix), y0, " ", c.border);
  }

  // --- interior geometry --------------------------------------------------------
  // Shared with the pixel image placement (graph_data::compute_graph_geom):
  // the legend/info rows, the graph rect (gx,gy,Wg,Hg), the ticks and the
  // dot columns — one computation, used by both renderers.
  const GraphGeom g = compute_graph_geom(x0, y0, W, H, mode, p,
                                         title_truncated, vmax);
  const int ix0 = g.ix0, iy0 = g.iy0, iw = g.iw, ih = g.ih;
  if (iw < 3 || ih < 1) return;
  const int legend_rows = g.legend_rows, ipr = g.ipr;
  const std::vector<int>& legend_idx = g.legend_idx;
  const int Hg = g.Hg, Wg = g.Wg, gx = g.gx, gy = g.gy;
  const std::vector<double>& fracs = g.fracs;
  const std::vector<std::string>& ticks = g.ticks;
  const bool have_graph = g.have_graph;
  const int dot_cols = g.dot_cols;

  // --- graph + grid ----------------------------------------------------------------
  // Text mode only: in pixel mode the backend draws the graph image at exactly
  // this rect one round later, so the text layer leaves it plain spaces
  // (the y axis, titles and legends stay text — the image sits beside them).
  if (have_graph && !pixels) {
    // Series that will actually draw (hidden series get no data): ONE -> the
    // area chart, SEVERAL -> one-dot braille lines with no fill.
    int visible = 0;
    for (const auto& sr : p.series)
      if (!sr.hidden) ++visible;

    std::vector<Decimated> decs(p.series.size());
    for (std::size_t i = 0; i < p.series.size(); ++i)
      if (!p.series[i].hidden) decs[i] = decimate(raws[i], dot_cols);

    std::vector<Cell> cells(static_cast<std::size_t>(Wg) * Hg);
    if (visible == 1) {
      for (std::size_t i = 0; i < p.series.size(); ++i) {
        if (p.series[i].hidden || !raws[i].valid()) continue;
        draw_area_cells(cells, Wg, Hg, decs[i], raws[i], vmax,
                        c.series_raw[p.series[i].color_index], c.mode);
        break;
      }
    } else {
      std::vector<GraphSeries> gs(p.series.size());
      for (std::size_t i = 0; i < p.series.size(); ++i) {
        if (p.series[i].hidden) continue;
        const int ci = p.series[i].color_index;
        gs[i] = GraphSeries{&decs[i], c.series[ci]};
      }
      draw_graph_cells(cells, Wg, Hg, gs, raws, vmax);
    }

    for (int cy = 0; cy < Hg; ++cy)
      for (int cc = 0; cc < Wg; ++cc) {
        const Cell& cell = cells[static_cast<std::size_t>(cy) * Wg + cc];
        if (cell.order < 0) continue;
        put_px(s, gx + cc, gy + cy,
               cell.glyph.empty() ? braille_char(cell.mask) : cell.glyph,
               cell.color);
      }
    // Gridlines: dim dotted "┈" at each tick row, only in cells with no data.
    for (std::size_t i = 0; i < fracs.size(); ++i) {
      const int ry = g.tick_row(i);
      for (int cc = 0; cc < Wg; ++cc)
        if (cells[static_cast<std::size_t>(ry) * Wg + cc].order < 0)
          put_px(s, gx + cc, gy + ry, "┈", c.grid);
    }
  }

  // --- y axis: the data area starts at gx on EVERY graph row; the separator
  //     column is grid-color "│", tick rows carry the right-aligned label and
  //     a border-color "┤" ("label ┤"), non-tick rows a blank label.
  if (have_graph) {
    for (int ry = 0; ry < Hg; ++ry)
      put_px(s, ix0 + g.label_w + 1, iy0 + ry, "│", c.grid);
    for (std::size_t i = 0; i < fracs.size(); ++i) {
      const int ry = g.tick_row(i);
      const std::string& lab = ticks[i];
      put_str(s, ix0 + (g.label_w - utf8_cols(lab)), iy0 + ry, lab,
              c.axis_text);
      put_px(s, ix0 + g.label_w + 1, iy0 + ry, "┤", c.border);
    }
  }

  // --- legend rows (below the graph) ---------------------------------------------
  // Full: 1/2/3 columns; each value is right-aligned inside its own column and
  // two blank columns separate the columns, so a value can never run into the
  // neighbour's "▌" ("12.00 GiB▌ Swap" is impossible). Compact: at most the
  // one truncation-rescue line (legend_rows > 0); Tight: none.
  {
    const int y0leg = iy0 + Hg;
    if (!full) {
      if (legend_rows > 0)
        draw_compact_legend(s, ix0, y0leg, iw, p, legend_values, c);
    } else {
      const int gutter = 2;
      std::vector<int> cw(ipr), cx(ipr);
      const int usable = iw - gutter * (ipr - 1);
      const int base = usable / ipr;
      const int extra = usable % ipr;
      int x = ix0;
      for (int j = 0; j < ipr; ++j) {
        cw[j] = base + (j < extra ? 1 : 0);
        cx[j] = x;
        x += cw[j] + gutter;
      }
      for (int k = 0; k < (int)legend_idx.size(); ++k) {
        const int si = legend_idx[k];
        const int col = k % ipr;
        draw_value_row(s, cx[col], y0leg + k / ipr, cw[col], true,
                       c.series[p.series[si].color_index],
                       p.series[si].label, legend_values[si], c.text, c.accent);
      }
    }
  }

  // --- info rows (Full mode only; Compact/Tight drop them) ------------------------
  if (full) {
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

// The graph window, for the header ("30s", "1m", "2m", "5m", "10m").
std::string window_str(int64_t window_ns) {
  const int s = (int)(window_ns / 1000000000LL);
  if (s < 60) return std::to_string(s) + "s";
  return std::to_string(s / 60) + "m";
}

// ---------------------------------------------------------------------------
// Phase 4 overlays. Both clear their rectangle with PLAIN SPACE cells (no
// background, no fill — the transparency guarantee) and draw a rounded
// border on top of the live frame underneath.
// ---------------------------------------------------------------------------

// A box centered on the screen: returns its top-left and inner size.
struct OverlayBox {
  int x0 = 0, y0 = 0, w = 0, h = 0;
  static OverlayBox make(int W, int H, int want_w, int want_h, int margin) {
    OverlayBox b;
    b.w = std::min(want_w, W - 2 * margin);
    if (b.w < 24) b.w = std::min(W, want_w);
    b.h = std::min(want_h, H - 2 * margin);
    if (b.h < 8) b.h = H;
    b.x0 = (W - b.w) / 2;
    b.y0 = (H - b.h) / 2;
    return b;
  }
};

void clear_box(ftxui::Screen& s, int x0, int y0, int w, int h) {
  const ftxui::Color def = ftxui::Color(ftxui::Color::Default);
  for (int y = y0; y < y0 + h; ++y)
    for (int x = x0; x < x0 + w; ++x) put_px(s, x, y, " ", def);
}

void box_border(ftxui::Screen& s, const OverlayBox& b, const std::string& title,
                const std::string& bottom, const Colors& c) {
  for (int x = 1; x + 1 < b.w; ++x) {
    put_px(s, b.x0 + x, b.y0, "─", c.border);
    put_px(s, b.x0 + x, b.y0 + b.h - 1, "─", c.border);
  }
  put_px(s, b.x0, b.y0, "╭", c.border);
  put_px(s, b.x0 + b.w - 1, b.y0, "╮", c.border);
  put_px(s, b.x0, b.y0 + b.h - 1, "╰", c.border);
  put_px(s, b.x0 + b.w - 1, b.y0 + b.h - 1, "╯", c.border);
  for (int y = 1; y + 1 < b.h; ++y) {
    put_px(s, b.x0, b.y0 + y, "│", c.border);
    put_px(s, b.x0 + b.w - 1, b.y0 + y, "│", c.border);
  }
  if (!title.empty()) put_str(s, b.x0 + 2, b.y0, " " + title, c.accent, true);
  if (!bottom.empty())
    put_str(s, b.x0 + 2, b.y0 + b.h - 1,
            truncate(" " + bottom, b.w - 4), c.label_dim);
}

// The panel manager: ~60 x min(rows-4, 30), centered, space-cleared.
void draw_manager_overlay(ftxui::Screen& s, int W, int H,
                          const ManagerOverlay& m, const Colors& c) {
  const OverlayBox b = OverlayBox::make(W, H, 60, std::min(H - 4, 30), 2);
  clear_box(s, b.x0, b.y0, b.w, b.h);
  box_border(s, b, "Panel Manager",
             "\xE2\x86\x91\xE2\x86\x93 select \xC2\xB7 space toggle \xC2\xB7 "
             "K/J move \xC2\xB7 r rename",
             c);
  // Interior: one hint row at the bottom, the rest for rows.
  const int inner_top = b.y0 + 1;
  const int inner_bottom = b.y0 + b.h - 2;  // hint row
  const int row_count = inner_bottom - inner_top;  // rows above the hint
  if (row_count < 1) return;
  const int x = b.x0 + 2;
  const int xw = b.w - 4;

  auto put_row = [&](int y, const std::string& mark, const ftxui::Color& mc,
                     const std::string& label, const ftxui::Color& lc,
                     bool bold, const std::string& suffix,
                     const ftxui::Color& sc) {
    put_str(s, x, y, mark + " ", mc);
    put_str(s, x + 2, y, truncate(label, std::max(0, xw - 2 - utf8_cols(suffix))), lc,
            bold);
    if (!suffix.empty())
      put_str(s, x + 2 + utf8_cols(truncate(label, std::max(
                                0, xw - 2 - utf8_cols(suffix)))),
              y, suffix, sc);
  };

  if (m.confirm_reset) {
    put_str(s, x, inner_top, "Reset panels to defaults?", c.text, true);
    put_str(s, x, inner_top + 1, "(y)es", c.accent, true);
    put_str(s, x + 8, inner_top + 1, "(n)o", c.label_dim);
    return;
  }

  if (m.submenu) {
    put_str(s, x, inner_top, truncate(m.submenu_title, xw), c.title, true);
    for (int k = 0; k < row_count - 1 && k < (int)m.sub_rows.size(); ++k) {
      const auto& r = m.sub_rows[k];
      const int y = inner_top + 1 + k;
      put_str(s, x, y, (r.hidden ? "\xC2\xB7 " : "\xE2\x9C\x93 "),
              r.hidden ? c.label_dim : c.series[0]);
      put_str(s, x + 2, y, truncate(r.label, xw - 2 - (r.hidden ? 9 : 0)),
              r.selected ? c.accent : c.text, r.selected);
      if (r.hidden) put_str(s, x + 2 + utf8_cols(truncate(r.label, xw - 11)), y,
                            " (hidden)", c.label_dim);
    }
    return;
  }

  const int n = (int)m.rows.size();
  int scroll = std::clamp(m.scroll, 0, std::max(0, n - row_count));
  for (int k = 0; k < row_count && scroll + k < n; ++k) {
    const auto& r = m.rows[scroll + k];
    const int y = inner_top + k;
    if (m.renaming && scroll + k == m.rename_row) {
      put_str(s, x, y, "\xE2\x9C\x93 ", c.series[0]);
      put_str(s, x + 2, y, "Rename: ", c.label_dim);
      const std::string buf = truncate(m.rename_buf, std::max(0, xw - 11));
      put_str(s, x + 10, y, buf, c.text);
      put_px(s, x + 10 + utf8_cols(buf), y, "\xE2\x96\x8C", c.accent, true);
      continue;
    }
    const std::string suffix = r.present ? "" : " (not present)";
    const ftxui::Color lc = r.selected ? c.accent
                                       : (r.present ? c.text : c.label_dim);
    put_row(y, (r.visible ? "\xE2\x9C\x93" : "\xC2\xB7"),
            r.visible ? c.series[0] : c.label_dim, r.label, lc, r.selected,
            suffix, c.label_dim);
  }
}

// The `?` help overlay: grouped key lines, any key closes.
void draw_help_overlay(ftxui::Screen& s, int W, int H, const HelpOverlay& h,
                       const Colors& c) {
  int n = (int)h.lines.size();
  const int max_rows = H - 4;
  if (n > max_rows) n = max_rows;
  const OverlayBox b = OverlayBox::make(W, H, 72, n + 2, 2);
  clear_box(s, b.x0, b.y0, b.w, b.h);
  box_border(s, b, h.title, "any key closes", c);
  const int x = b.x0 + 2;
  const int xw = b.w - 4;
  int keys_w = 0;
  for (int i = 0; i < n; ++i) keys_w = std::max(keys_w, utf8_cols(h.lines[i].first));
  keys_w = std::min(keys_w, xw / 2);
  for (int i = 0; i < n; ++i) {
    const int y = b.y0 + 1 + i;
    put_str(s, x, y, h.lines[i].first + std::string(std::max(0, keys_w - utf8_cols(h.lines[i].first)) + 2, ' '), c.accent);
    put_str(s, x + keys_w + 2, y, truncate(h.lines[i].second, std::max(0, xw - keys_w - 2)), c.text);
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
                  bool paused, long long interval_ms, LayoutCache& cache,
                  int top_row, int64_t window_ns, const FrameChrome& chrome,
                  int focus_panel, const ManagerOverlay* manager,
                  const HelpOverlay* help) {
  screen.Clear();
  const int W = screen.dimx(), H = screen.dimy();
  const Colors c = build_colors(theme, mode);
  const Snapshot& snap = frozen ? frozen->snap : latest;

  // Phase 5 solid mode: paint the theme's bg across every pixel. This is the
  // ONLY sanctioned background-color path in GETH — every other draw writes
  // foregrounds only.
  auto paint_solid_bg = [&] {
    if (!chrome.solid_bg) return;
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x)
        screen.PixelAt(x, y).background_color = c.bg;
  };

  // Small terminals: never crash, show a centered notice below 40x12.
  if (W < 40 || H < 12) {
    const std::string shown = truncate("GETH — terminal too small (need 40×12)", W);
    put_str(screen, (W - utf8_cols(shown)) / 2, H / 2, shown, c.text);
    paint_solid_bg();
    return;
  }

  // Responsive layout: recomputed only when the terminal size changed.
  const Layout& lo = cache.get(W, H);
  const int content_h = H - 2;
  // The visible window: every row when not scrolling; k = lo.rows()
  // consecutive grid rows (offset tr) when scrolling. The k rows are
  // stretched to fill the viewport exactly (row_heights sums to content_h),
  // so the visible area is tiled with no blank rows; visible row r starts at
  // terminal row 1 + the sum of the visible rows' heights before it.
  const int tr =
      lo.scrolling ? std::clamp(top_row, 0, max_top_row(lo, content_h)) : 0;
  const int last_visible =
      lo.scrolling ? std::min(lo.total_rows, tr + lo.rows()) : lo.rows();

  const TimePoint now = Clock::now();

  auto render_one = [&](int pi, int px, int py, int pw, int ph, PanelMode pm) {
    const PanelSpec& p = page[pi];
    std::vector<std::vector<int64_t>> ts(p.series.size());
    std::vector<std::vector<float>> vs(p.series.size());
    std::vector<RawWindow> raws(p.series.size());
    if (frozen) {
      for (std::size_t i = 0; i < p.series.size(); ++i) {
        if (p.series[i].hidden) continue;  // no data, no line, no legend
        if (const FrozenFrame::Window* w = frozen->find(p.series[i].id))
          raws[i] = RawWindow{w->t.data(), w->v.data(), w->t.size(), w->t_start,
                              w->t_end, w->span_ns};
      }
    } else {
      for (std::size_t i = 0; i < p.series.size(); ++i) {
        if (p.series[i].hidden) continue;
        load_live(raws[i], ts[i], vs[i], store, p.series[i].id, window_ns);
      }
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
    if (trace::active())
      for (std::size_t i = 0; i < p.series.size(); ++i)
        if (trace::matches(p.series[i].id))
          trace::stage("panel", p.series[i].id,
                       p.title + ": " + p.series[i].label + " = " + legend_values[i]);

    draw_panel(screen, px, py, pw, ph, p, pm, sleeping, raws, legend_values, snap,
               vmax, c, chrome.pixels, pi == focus_panel);
  };

  // --- panel grid: the visible rows of the layout -------------------------------
  // lo.panels is parallel to the VISIBLE panels; map back to the page index.
  // When not scrolling the stored rect is already screen-ready (rect.y is an
  // offset from the top of the content area). When scrolling, a panel's
  // on-screen y, height and mode all depend on the current offset, so they
  // are recomputed from the k stretched visible rows (the stored rect is
  // only the offset-0 form, which is never drawn in scroll mode).
  for (int r = tr; r < last_visible; ++r)
    for (int i = 0; i < (int)lo.panels.size(); ++i)
      if (lo.panels[i].rect.row == r) {
        const auto& pl = lo.panels[i];
        const int pi = cache.visible_idx[i];
        int py = 1 + pl.rect.y;
        int ph = pl.rect.h;
        PanelMode pm = pl.mode;
        if (lo.scrolling) {
          const int vis = r - tr;  // 0 .. k-1 within the visible window
          int top = 1;
          for (int q = 0; q < vis; ++q) top += lo.row_heights[q];
          py = top;
          ph = lo.row_heights[vis];
          PanelMode m2;
          pm = best_mode(page[pi], pl.rect.w, ph, m2) ? m2 : PanelMode::kTight;
        }
        render_one(pi, pl.rect.x, py, pl.rect.w, ph, pm);
      }

  // --- header: <CPU> & <dGPU>  [PAUSED] <interval> <window> <time> --------------
  // Painted AFTER the panels so a row cut off at the top can never bleed into it.
  // [general].title (non-empty) replaces the auto title entirely.
  std::string htitle;
  if (!chrome.title_override.empty()) {
    htitle = chrome.title_override;
  } else {
    htitle = names::short_cpu(hw.cpu_name);
    for (const auto& g : hw.gpus) {
      if (!g.is_integrated) {
        htitle += " & " + gpu_short_name(g);
        break;  // primary dGPU only
      }
    }
  }
  {
    for (int x = 0; x < W; ++x) put_px(screen, x, 0, " ", c.label_dim);
    const std::string rtime = time_str();
    const std::string rmid =
        interval_str(interval_ms) + " \xC2\xB7 " + window_str(window_ns) + " ";
    const std::string rpaused = paused ? "PAUSED " : "";
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

  // --- footer --------------------------------------------------------------------
  {
    for (int x = 0; x < W; ++x) put_px(screen, x, H - 1, " ", c.label_dim);
    std::string foot = "q quit \xC2\xB7 space pause \xC2\xB7 t window \xC2\xB7 e panels \xC2\xB7 ? help";
    // Scrolling: "· ↑↓ scroll · [▲ ]N more[ ▼]" — N is the number of hidden
    // rows (total - k, constant while scrolling); "▲ " only when there are
    // hidden rows above, " ▼" only when there are hidden rows below. This
    // tail must ALWAYS survive right-truncation, so on narrow terminals the
    // key hints (foot) are the part that gets cut, never the scroll info.
    std::string scroll_part;
    if (lo.scrolling) {
      const int k = lo.rows();  // visible rows
      const int hidden = lo.total_rows - k;
      const bool at_top = (tr == 0);
      const bool at_bottom = (tr == lo.total_rows - k);
      scroll_part = " \xC2\xB7 \xE2\x86\x91\xE2\x86\x93 scroll";  // "· ↑↓ scroll"
      if (hidden > 0)
        scroll_part += std::string(" \xC2\xB7 ") +
                       (at_top ? "" : "\xE2\x96\xB2 ") +  // "▲ "
                       std::to_string(hidden) + " more" +
                       (at_bottom ? "" : " \xE2\x96\xBC");  // " ▼"
    }
    const int scroll_w = utf8_cols(scroll_part);
    // Truncate the key hints to reserve the scroll tail's full width.
    const std::string shown_foot =
        scroll_w > 0 ? truncate(foot, std::max(0, W - scroll_w)) : foot;
    // Right side: a broken config is warned about PERMANENTLY; otherwise a
    // transient toast (e.g. "saved") while now < toast_until. It sits between
    // the hints and the scroll tail, and is dropped when no room is left.
    const bool toast_live =
        !chrome.toast.empty() && Clock::now() < chrome.toast_until;
    if (!chrome.config_warning.empty() || toast_live) {
      const std::string msg = chrome.config_warning.empty()
                                  ? chrome.toast
                                  : "config error (" + chrome.config_warning +
                                        ") \xE2\x80\x94 using defaults, not saving";
      const ftxui::Color mc =
          chrome.config_warning.empty() ? c.text : c.warn;
      const int room = W - scroll_w - utf8_cols(shown_foot) - 2;
      if (room > 6) {
        const std::string shown = truncate(msg, room);
        put_str(screen, W - scroll_w - utf8_cols(shown) - 1, H - 1, shown, mc);
      }
    }
    put_str(screen, 0, H - 1, shown_foot, c.label_dim);
    if (scroll_w > 0) put_str(screen, W - scroll_w, H - 1, scroll_part, c.label_dim);
  }

  // --- Phase 4 overlays (last, on top of everything) ---------------------------
  if (manager) draw_manager_overlay(screen, W, H, *manager, c);
  if (help) draw_help_overlay(screen, W, H, *help, c);

  // Solid mode: the background goes down last. Every pixel still shows the
  // content drawn above — only the (default) cell background is replaced.
  paint_solid_bg();
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
                                   long long interval_ms, LayoutCache& cache,
                                   int top_row, int64_t window_ns,
                                   const FrameChrome& chrome, int focus_panel,
                                   const ManagerOverlay* manager,
                                   const HelpOverlay* help) {
  // The Element outlives this call (FTXUI renders it after the caller's
  // OnRender has returned), so the chrome / overlays are COPIED into the
  // closure: their owners are caller locals. (hw / page / store / latest /
  // state / theme / cache are captured by reference; their owners outlive the
  // element in every call path.)
  FrameChrome chrome_copy = chrome;
  ManagerOverlay manager_copy;
  HelpOverlay help_copy;
  const bool has_manager = manager != nullptr;
  const bool has_help = help != nullptr;
  if (has_manager) manager_copy = *manager;
  if (has_help) help_copy = *help;
  return std::make_shared<FrameNode>(
      [&hw, &page, &store, &latest, frozen, &state, &theme, mode, paused,
       interval_ms, &cache, top_row, window_ns, focus_panel, chrome_copy,
       manager_copy, help_copy, has_manager,
       has_help](ftxui::Screen& screen) {
        render_frame(screen, hw, page, store, latest, frozen, state, theme, mode,
                     paused, interval_ms, cache, top_row, window_ns,
                     chrome_copy, focus_panel,
                     has_manager ? &manager_copy : nullptr,
                     has_help ? &help_copy : nullptr);
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

namespace {
std::atomic<long long> g_process_start_ns{0};
}  // namespace

void frame_mark_process_start() {
  g_process_start_ns.store(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          Clock::now().time_since_epoch())
          .count());
}

long long process_start_ns() { return g_process_start_ns.load(); }

void FrameStats::print() const {
  std::printf("geth stats: %llu frames, %llu samples\n", frames, samples);
  if (first_frame_ms >= 0)
    std::printf("  first frame   %9.0f ms after process start\n",
                first_frame_ms);
  std::printf("  render time   mean %9.1f us  max %10.1f us\n", render_us_mean(),
              render_us_max);
  std::printf("  sample time   mean %9.1f us  max %10.1f us\n", sample_us_mean(),
              sample_us_max);
  std::printf("  allocs/frame  mean %9.1f    max %10lld\n", allocs_mean(), allocs_max);
}

FrozenFrame capture_frozen(const std::vector<PanelSpec>& page,
                           const SeriesStore& store, const Snapshot& latest,
                           int64_t window_ns) {
  FrozenFrame f;
  f.snap = latest;
  for (const auto& p : page)
    for (const auto& sr : p.series) {
      if (f.find(sr.id)) continue;
      FrozenFrame::Window w;
      w.id = sr.id;
      RawWindow raw;
      load_live(raw, w.t, w.v, store, sr.id, window_ns);
      if (!raw.valid()) continue;
      w.t_start = raw.t_start;
      w.t_end = raw.t_end;
      w.span_ns = raw.span_ns;
      f.windows.push_back(std::move(w));
    }
  return f;
}

}  // namespace geth::ui
