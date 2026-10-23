// Rasterizer tests (v1.1.0 Part A): raster_panel turns one graph rect into an
// RGBA8 image. These check the geometry and the layering rules directly,
// without the terminal — a horizontal line lands on the formula's pixel row,
// the anti-aliasing produces partial alpha, the fill fades toward the
// baseline, the background is transparent, a NaN gap breaks line AND fill, and
// the gridlines land on the SAME rows the text tick labels sit on.
#include <doctest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "ui/graph_data.hpp"
#include "ui/graph_image.hpp"
#include "ui/panel_spec.hpp"
#include "ui/theme.hpp"

using namespace geth;
using namespace geth::ui;

namespace {

constexpr int kCellW = 7, kCellH = 18;  // a typical terminal cell (px)

// A minimal theme with known, distinct colors so the assertions are
// deterministic and independent of the shipped palettes.
Theme test_theme() {
  Theme t;
  t.grid = rgb(120, 120, 120);
  t.series[0] = rgb(255, 0, 0);  // red (primary)
  t.series[1] = rgb(0, 255, 0);  // green
  t.bg = rgb(8, 10, 14);
  return t;
}

// A graph rect Wg x Hg, absolute scale 0..vmax, ticks at 100/50/0 %.
GraphGeom geom_basic(int Wg, int Hg, double vmax) {
  GraphGeom g;
  g.W = Wg + 10;
  g.H = Hg + 4;
  g.ix0 = 2;
  g.iy0 = 1;
  g.iw = Wg + 4;
  g.ih = Hg;
  g.gx = 6;
  g.gy = 1;
  g.Wg = Wg;
  g.Hg = Hg;
  g.have_graph = true;
  g.dot_cols = 2 * Wg;
  g.vmax = vmax;
  g.fracs = {1.0, 0.5, 0.0};
  g.ticks = {"100", "50", "0"};
  g.label_w = 3;
  return g;
}

// A constant-value single series over [t0, t1] (n decimated points). Fills
// both the RawWindow view and the Decimated arrays (they share the buffers).
void const_series(Decimated& d, RawWindow& w, int64_t t0, int64_t t1, double val,
                  int n) {
  d.t.resize((std::size_t)n);
  d.v.resize((std::size_t)n);
  for (int i = 0; i < n; ++i) {
    d.t[i] = t0 + (int64_t)((double)(t1 - t0) * i / (n - 1));
    d.v[i] = (float)val;
  }
  w.t = d.t.data();
  w.v = d.v.data();
  w.n = (std::size_t)n;
  w.t_start = t0;
  w.t_end = t1;
  w.span_ns = (double)(t1 - t0);
}

SeriesSpec series0(bool hidden = false) {
  SeriesSpec s;
  s.id = "test.cpu";
  s.label = "CPU";
  s.color_index = 0;
  s.hidden = hidden;
  return s;
}

int alpha(const RasterImage& img, int x, int y) { return img.at(x, y)[3]; }

}  // namespace

TEST_CASE("rasterizer: a horizontal line lands on the formula's pixel row") {
  const int Wg = 20, Hg = 7;
  const double vmax = 128.0;
  const GraphGeom g = geom_basic(Wg, Hg, vmax);
  const int w = Wg * kCellW, h = Hg * kCellH;

  // A constant 64 on a 0..128 scale. value_y_px(64) = cell_h/2 + (1-64/128)*
  // (Hg-1)*cell_h = 9 + 0.5*6*18 = 9 + 54 = 63 (an integer here).
  Decimated d;
  RawWindow rw;
  const_series(d, rw, 0, 6000000000LL, 64.0, 40);
  std::vector<RawWindow> raws = {rw};
  std::vector<Decimated> decs = {d};
  std::vector<SeriesSpec> series = {series0()};

  const RasterImage img = raster_panel(
      {g, kCellW, kCellH, test_theme(), false, raws, decs, series});
  REQUIRE_EQ(img.w, w);
  REQUIRE_EQ(img.h, h);

  const int x = w / 2;  // a data column
  // The line (width max(1.5, cell_h/9) = 2) is centered on y = 63, so the two
  // nearest pixel rows (62, 63) carry it at full strength.
  CHECK_EQ(alpha(img, x, 62), 255);
  CHECK_EQ(alpha(img, x, 63), 255);
  // A row well above the line is empty (no line, no fill above the line).
  CHECK_EQ(alpha(img, x, 5), 0);
  CHECK_EQ(alpha(img, x, 61), 0);
}

TEST_CASE("rasterizer: anti-aliasing yields partial alpha on a slanted line") {
  const int Wg = 40, Hg = 7;
  const double vmax = 128.0;
  const GraphGeom g = geom_basic(Wg, Hg, vmax);

  // A ramp 0 -> vmax across the window: the line climbs diagonally, so its
  // edge crosses pixel rows with partial coverage.
  const int n = 80;
  Decimated d;
  d.t.resize((std::size_t)n);
  d.v.resize((std::size_t)n);
  RawWindow rw;
  for (int i = 0; i < n; ++i) {
    const double u = (double)i / (n - 1);
    d.t[i] = (int64_t)(6000000000LL * u);
    d.v[i] = (float)(vmax * u);
  }
  rw.t = d.t.data();
  rw.v = d.v.data();
  rw.n = (std::size_t)n;
  rw.t_start = 0;
  rw.t_end = 6000000000LL;
  rw.span_ns = 6000000000.0;
  std::vector<RawWindow> raws = {rw};
  std::vector<Decimated> decs = {d};
  std::vector<SeriesSpec> series = {series0()};

  const RasterImage img = raster_panel(
      {g, kCellW, kCellH, test_theme(), false, raws, decs, series});
  int full = 0, partial = 0;
  for (int y = 0; y < img.h; ++y)
    for (int x = 0; x < img.w; ++x) {
      const int a = alpha(img, x, y);
      if (a == 255) ++full;
      else if (a > 0) ++partial;
    }
  CHECK(full > 0);    // the line core is opaque
  CHECK(partial > 0);  // the AA edge is semi-transparent
}

TEST_CASE("rasterizer: the fill fades from the line toward the baseline") {
  const int Wg = 20, Hg = 7;
  const double vmax = 128.0;
  const GraphGeom g = geom_basic(Wg, Hg, vmax);
  const int w = Wg * kCellW;

  // A constant 64 -> the line sits at y = 63; the fill runs from there to the
  // bottom (y = h-1), fading 35 % -> 5 %.
  Decimated d;
  RawWindow rw;
  const_series(d, rw, 0, 6000000000LL, 64.0, 40);
  std::vector<RawWindow> raws = {rw};
  std::vector<Decimated> decs = {d};
  std::vector<SeriesSpec> series = {series0()};

  const RasterImage img = raster_panel(
      {g, kCellW, kCellH, test_theme(), false, raws, decs, series});
  const int x = w / 2;
  // Rows strictly below the line carry only the fill. Sample three of them
  // (none is a gridline row: the ticks are at y = 9, 63, 117).
  const int a_top = alpha(img, x, 70);
  const int a_mid = alpha(img, x, 90);
  const int a_bot = alpha(img, x, 120);
  CHECK(a_top > 0);
  CHECK(a_mid > 0);
  CHECK(a_bot > 0);
  CHECK(a_top > a_mid);  // fading toward the baseline
  CHECK(a_mid > a_bot);
  CHECK(a_top <= (int)(0.35 * 255 + 1));  // never stronger than the 35 % peak
}

TEST_CASE("rasterizer: the background is transparent by default") {
  const int Wg = 20, Hg = 7;
  const GraphGeom g = geom_basic(Wg, Hg, 128.0);
  const int w = Wg * kCellW;

  // No data at all: the image is just the (transparent) background plus the
  // gridlines. A pixel that is neither a gridline nor a line must be fully
  // transparent.
  std::vector<RawWindow> raws = {{nullptr, nullptr, 0, 0, 0, 0.0}};
  std::vector<Decimated> decs = {{{}, {}}};
  std::vector<SeriesSpec> series = {series0()};  // no data -> no line

  const RasterImage img = raster_panel(
      {g, kCellW, kCellH, test_theme(), false, raws, decs, series});
  // (0,0) is above every line and not a gridline row (ticks at 9, 63, 117).
  CHECK_EQ(alpha(img, 0, 0), 0);
  CHECK_EQ(alpha(img, w / 2, 30), 0);  // a mid rect, non-tick row

  // Solid mode fills the background with the theme's bg at full alpha.
  const RasterImage solid = raster_panel(
      {g, kCellW, kCellH, test_theme(), true, raws, decs, series});
  CHECK_EQ(alpha(solid, 0, 0), 255);
  const Rgb bg = test_theme().bg;
  CHECK_EQ((int)solid.at(0, 0)[0], (int)bg.r);
  CHECK_EQ((int)solid.at(0, 0)[1], (int)bg.g);
  CHECK_EQ((int)solid.at(0, 0)[2], (int)bg.b);
}

TEST_CASE("rasterizer: a NaN gap breaks both the line and the fill") {
  const int Wg = 20, Hg = 7;
  const double vmax = 128.0;
  const GraphGeom g = geom_basic(Wg, Hg, vmax);
  const int w = Wg * kCellW, h = Hg * kCellH;

  // Data on the left and right, a NaN hole in the middle (t = 300 of 0..600).
  const std::vector<int64_t> t = {0, 1000000000LL, 2000000000LL, 3000000000LL,
                                  4000000000LL, 5000000000LL, 6000000000LL};
  const std::vector<float> v = {100, 100, 100, 0, 100, 100, 100};  // 0 marks gap
  Decimated d;
  d.t = t;
  d.v.resize(t.size());
  for (std::size_t i = 0; i < t.size(); ++i)
    d.v[i] = (v[i] == 0) ? std::nan("") : v[i];
  RawWindow rw;
  rw.t = d.t.data();
  rw.v = d.v.data();
  rw.n = d.t.size();
  rw.t_start = t.front();
  rw.t_end = t.back();
  rw.span_ns = (double)(t.back() - t.front());
  std::vector<RawWindow> raws = {rw};
  std::vector<Decimated> decs = {d};
  std::vector<SeriesSpec> series = {series0()};

  const RasterImage img = raster_panel(
      {g, kCellW, kCellH, test_theme(), false, raws, decs, series});
  // The line (value 100) sits near y = value_y_px(100) = 9 + (1-100/128)*108 =
  // 9 + 23.6 = 32.6, so rows 32/33. The gap is at the center column.
  auto col_sum = [&](int x) {
    int s = 0;
    for (int y = 0; y < h; ++y) s += alpha(img, x, y);
    return s;
  };
  // The data column (far left) has line + fill; the gap column (center) has
  // only the gridlines. The data column must carry far more ink.
  CHECK(col_sum(2) > col_sum(w / 2));
  // At the gap, the line row itself is empty (the line is broken there).
  CHECK_EQ(alpha(img, w / 2, 32), 0);
  CHECK_EQ(alpha(img, w / 2, 33), 0);
}

TEST_CASE("rasterizer: gridlines land on the text tick-label rows") {
  const int Wg = 20, Hg = 7;
  const GraphGeom g = geom_basic(Wg, Hg, 128.0);
  const int w = Wg * kCellW;

  // A hidden series -> no line, no fill: the image is background + gridlines
  // only, so each tick row can be read cleanly.
  std::vector<RawWindow> raws = {{nullptr, nullptr, 0, 0, 0, 0.0}};
  std::vector<Decimated> decs = {{{}, {}}};
  std::vector<SeriesSpec> series = {series0(/*hidden=*/true)};

  const RasterImage img = raster_panel(
      {g, kCellW, kCellH, test_theme(), false, raws, decs, series});
  const Rgb grid = test_theme().grid;
  const int x = w / 2;
  for (std::size_t i = 0; i < g.fracs.size(); ++i) {
    const int row = g.tick_y_px(i, kCellH);  // the SAME formula the text uses
    REQUIRE(row >= 0);
    REQUIRE(row < img.h);
    CHECK_EQ(alpha(img, x, row), 153);  // the grid's 60 % alpha
    CHECK_EQ((int)img.at(x, row)[0], (int)grid.r);
    CHECK_EQ((int)img.at(x, row)[1], (int)grid.g);
    CHECK_EQ((int)img.at(x, row)[2], (int)grid.b);
  }
  // A non-tick row is empty.
  CHECK_EQ(alpha(img, x, 30), 0);
}

// v1.5.3: decimate keeps BOTH a column's min and max (emitted in time order),
// so a one-sample dip survives as strongly as a one-sample spike. The old
// max-only rule dropped the dip. A 10 m window of 1-s samples at 40 dot
// columns is ~15 samples per column — dense enough that a single-sample dip or
// spike sits wholly inside one column.
TEST_CASE("decimate: a one-sample dip and a one-sample spike both survive a dense 10m column") {
  const int64_t step = 1000000000LL;  // 1 s
  const int N = 601;                   // 0..600 s = a 10 m window
  std::vector<int64_t> t(N);
  std::vector<float> v(N, 64.0f);      // a constant baseline on 0..128
  for (int i = 0; i < N; ++i) t[i] = (int64_t)i * step;
  const int dip_i = 200, spike_i = 400;  // each wholly inside a dense column
  v[dip_i] = 16.0f;     // the one-sample dip
  v[spike_i] = 112.0f;  // the one-sample spike
  const int64_t span_ns = (int64_t)(N - 1) * step;
  const RawWindow w{t.data(), v.data(), (std::size_t)N, 0, span_ns,
                    (double)span_ns};
  const int Wg = 20;  // dot_cols = 2 * Wg = 40
  const Decimated d = decimate(w, 2 * Wg);

  // (core) both extremes survive the decimation (max-only would drop the dip).
  float lo = 1e9f, hi = -1e9f;
  for (const float x : d.v)
    if (std::isfinite(x)) lo = std::min(lo, x), hi = std::max(hi, x);
  CHECK(lo <= 16.0f + 1e-3f);
  CHECK(hi >= 112.0f - 1e-3f);

  // (text) value_at at the dip's and the spike's times returns the extreme,
  // not the baseline — the text line passes through both.
  CHECK(value_at(d, (double)t[dip_i]) < 40.0);
  CHECK(value_at(d, (double)t[spike_i]) > 80.0);

  // (pixel) raster_panel draws both: the line reaches the spike's row (near the
  // top) and the dip's row (near the bottom), which the baseline (row ~63)
  // never does. value_y_px(y) = cell_h/2 + (1 - y/vmax)*(Hg-1)*cell_h, Hg=7,
  // cell_h=18, vmax=128: baseline 64 -> ~63, spike 112 -> ~22, dip 16 -> ~104.
  const GraphGeom g = geom_basic(Wg, 7, 128.0);
  const int W = Wg * kCellW, H = 7 * kCellH;
  std::vector<RawWindow> raws = {w};
  std::vector<Decimated> decs = {d};
  std::vector<SeriesSpec> series = {series0()};
  const RasterImage img = raster_panel(
      {g, kCellW, kCellH, test_theme(), false, raws, decs, series});
  REQUIRE_EQ(img.w, W);
  REQUIRE_EQ(img.h, H);
  auto row_max = [&](int r0, int r1) {
    int m = 0;
    for (int x = 0; x < W; ++x)
      for (int y = r0; y < r1 && y < H; ++y) m = std::max(m, alpha(img, x, y));
    return m;
  };
  CHECK(row_max(16, 28) > 15);    // the spike line reaches near the top
  CHECK(row_max(99, 110) > 15);   // the dip line reaches near the bottom
  CHECK(row_max(58, 68) > 15);    // the baseline line (sanity: it is present)
}
