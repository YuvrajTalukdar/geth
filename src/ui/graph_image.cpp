#include "ui/graph_image.hpp"

#include <algorithm>
#include <cmath>

namespace geth::ui {

namespace {

// Straight-alpha "over" compositing (8-bit), destination in place.
// out_c = (c_s*a_s + c_d*a_d*(1-a_s)) / out_a, out_a = a_s + a_d*(1-a_s).
void blend_over(std::uint8_t* px, int r, int g, int b, int a) {
  a = std::clamp(a, 0, 255);
  if (a <= 0) return;
  const int da = px[3];
  const int oa = a + (da * (255 - a)) / 255;
  if (oa <= 0) return;
  const int ws = a * 255;
  const int wd = da * (255 - a);
  px[0] = (std::uint8_t)((r * ws + px[0] * wd) / (oa * 255));
  px[1] = (std::uint8_t)((g * ws + px[1] * wd) / (oa * 255));
  px[2] = (std::uint8_t)((b * ws + px[2] * wd) / (oa * 255));
  px[3] = (std::uint8_t)oa;
}

// Distance from (px,py) to the segment (x0,y0)-(x1,y1).
double seg_dist(double px, double py, double x0, double y0, double x1,
                double y1) {
  const double dx = x1 - x0, dy = y1 - y0;
  const double l2 = dx * dx + dy * dy;
  double t = l2 > 0.0 ? ((px - x0) * dx + (py - y0) * dy) / l2 : 0.0;
  t = std::clamp(t, 0.0, 1.0);
  const double cx = x0 + t * dx, cy = y0 + t * dy;
  return std::hypot(px - cx, py - cy);
}

// One line in pixel space: the sample points P[k] = (k/2, y[k]) — twice the
// pixel-column count, in time order — NaN = gap. y is the value_y_px of the
// clamped value (v > vmax clamps to the top row's center, like the text
// renderer's lround mapping).
struct LineSamples {
  std::vector<double> y;  // size 2*w, NaN = gap
  bool has_data = false;
};

LineSamples sample_line(const Decimated& d, const RawWindow& w, int wpx,
                        double vmax, const GraphGeom& g, int cell_h) {
  LineSamples out;
  if (wpx < 1 || vmax <= 0.0 || !w.valid() || d.t.empty()) return out;
  const int n2 = 2 * wpx;
  out.y.assign((std::size_t)n2, std::nan(""));
  const auto& tt = d.t;
  const auto& vv = d.v;
  const std::size_t n = tt.size();
  // Running index: the sampled times advance monotonically, so the
  // decimated pointer only moves forward (one pass, like value_at).
  std::size_t p = 0;
  for (int k = 0; k < n2; ++k) {
    const double t =
        w.t_start + w.span_ns * (n2 > 1 ? k / (double)(n2 - 1) : 1.0);
    while (p + 1 < n && t >= (double)tt[p + 1]) ++p;
    double v;
    if (n == 1)
      v = std::abs(t - (double)tt[0]) < 1e3 ? vv[0] : std::nan("");
    else if (p + 1 >= n)
      v = std::abs(t - (double)tt[n - 1]) < 1e3 ? vv[n - 1] : std::nan("");
    else {
      const double t0 = tt[p], t1 = tt[p + 1];
      if (!std::isfinite(vv[p]) || !std::isfinite(vv[p + 1]))
        v = std::nan("");
      else if (t1 <= t0)
        v = vv[p + 1];
      else
        v = vv[p] + ((t - t0) / (t1 - t0)) * (vv[p + 1] - vv[p]);
    }
    if (!std::isfinite(v)) continue;
    out.has_data = true;
    out.y[k] = g.value_y_px(std::clamp(v, 0.0, vmax), cell_h);
  }
  return out;
}

}  // namespace

RasterImage raster_panel(const RasterArgs& a) {
  RasterImage out;
  const GraphGeom& g = a.geom;
  const int w = g.Wg * a.cell_w, h = g.Hg * a.cell_h;
  if (w < 1 || h < 1 || a.cell_w < 1 || a.cell_h < 1) return out;
  out.w = w;
  out.h = h;
  out.rgba.assign((std::size_t)w * h * 4, 0);

  // 1. Background: transparent by default, the theme's bg in solid mode.
  if (a.solid_bg) {
    const Rgb& bg = a.theme.bg;
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x) {
        std::uint8_t* px = out.at(x, y);
        px[0] = bg.r;
        px[1] = bg.g;
        px[2] = bg.b;
        px[3] = 255;
      }
  }

  // 2. Gridlines: 1 px at each tick row (the SAME row the text tick label
  //    sits on), the theme's grid at 60 % alpha.
  for (std::size_t i = 0; i < g.fracs.size(); ++i) {
    const int row =
        std::clamp((int)std::lround(g.tick_y_px(i, a.cell_h)), 0, h - 1);
    for (int x = 0; x < w; ++x)
      blend_over(out.at(x, row), a.theme.grid.r, a.theme.grid.g,
                 a.theme.grid.b, 153);
  }

  // Which series draw, in DRAW order: the secondaries first (legend order),
  // the primary (series 0) last, so it sits on top.
  std::vector<int> order;
  for (std::size_t i = 1; i < a.series.size(); ++i)
    if (!a.series[i].hidden) order.push_back((int)i);
  if (!a.series.empty() && !a.series[0].hidden) order.push_back(0);
  if (order.empty()) return out;

  std::vector<LineSamples> ls(order.size());
  for (std::size_t i = 0; i < order.size(); ++i)
    ls[i] = sample_line(a.decs[order[i]], a.raws[order[i]], w, g.vmax, g,
                        a.cell_h);

  // 3. The fill under the first line that has data: 35 % alpha at the line
  //    -> 5 % at the image bottom (halved at 3+ series); a NaN gap leaves
  //    the column unfilled (never a 0-floor).
  int fill_i = -1;  // index into `ls` (the palette section reuses it)
  for (std::size_t i = 0; i < order.size(); ++i)
    if (ls[i].has_data) {
      fill_i = (int)i;
      break;
    }
  if (fill_i >= 0) {
    const Rgb& fc = a.theme.series[a.series[order[fill_i]].color_index];
    const double strength = order.size() >= 3 ? 0.5 : 1.0;
    const std::vector<double>& fy = ls[fill_i].y;
    for (int x = 0; x < w; ++x) {
      const double ytop = fy[2 * x + 1];  // the line at the column center
      if (!std::isfinite(ytop)) continue;
      const double top = std::clamp(ytop, 0.0, (double)h);
      const int py0 = std::max(0, (int)std::ceil(top - 1.0));
      for (int py = py0; py < h; ++py) {
        const double frac = std::clamp(py + 1.0 - top, 0.0, 1.0);
        // The gradient at the pixel center: 35 % at the line, 5 % at the
        // bottom of the image.
        const double span = h > (int)top ? (double)h - top : 1.0;
        const double grad = 0.35 +
            (0.05 - 0.35) * std::clamp((py + 0.5 - top) / span, 0.0, 1.0);
        const int alpha = (int)std::lround(255.0 * strength * grad * frac);
        blend_over(out.at(x, py), fc.r, fc.g, fc.b, alpha);
      }
    }
  }

  // 4. The lines: coverage-anti-aliased capsules (width max(1.5, cell_h/9)),
  //    the series color at full strength; the row range per column is the
  //    segment band, so flat lines touch a handful of rows, not all of h.
  const double width = std::max(1.5, (double)a.cell_h / 9.0);
  for (std::size_t si = 0; si < order.size(); ++si) {
    if (!ls[si].has_data) continue;
    const Rgb& col = a.theme.series[a.series[order[si]].color_index];
    const std::vector<double>& L = ls[si].y;
    const int ns = (int)L.size();
    for (int x = 0; x < w; ++x) {
      // The half-pixel segments S[k] (P[k] -> P[k+1]) that cross pixel
      // column [x, x+1): k = 2x-1, 2x, 2x+1.
      double sx[3][4];
      double ylo = 1e9, yhi = -1e9;
      int nseg = 0;
      for (int k = 2 * x - 1; k <= 2 * x + 1; ++k) {
        if (k < 0 || k + 1 >= ns) continue;
        if (!std::isfinite(L[k]) || !std::isfinite(L[k + 1])) continue;
        sx[nseg][0] = k / 2.0;
        sx[nseg][1] = L[k];
        sx[nseg][2] = (k + 1) / 2.0;
        sx[nseg][3] = L[k + 1];
        ylo = std::min(ylo, std::min(L[k], L[k + 1]));
        yhi = std::max(yhi, std::max(L[k], L[k + 1]));
        ++nseg;
      }
      if (nseg == 0) continue;
      const int py0 = std::max(0, (int)std::floor(ylo - width));
      const int py1 = std::min(h - 1, (int)std::ceil(yhi + width));
      for (int py = py0; py <= py1; ++py) {
        double best = 1e9;
        for (int s2 = 0; s2 < nseg; ++s2)
          best = std::min(best, seg_dist(x + 0.5, py + 0.5, sx[s2][0],
                                         sx[s2][1], sx[s2][2], sx[s2][3]));
        const double cov = std::clamp(0.5 + (width * 0.5 - best), 0.0, 1.0);
        if (cov <= 0.0) continue;
        blend_over(out.at(x, py), col.r, col.g, col.b,
                   (int)std::lround(255.0 * cov));
      }
    }
  }

  // The sixel encoder's palette (the kitty encoder ignores it): the grid,
  // the series colors in draw order, the fill color last (its gradient
  // steps are derived from it).
  out.palette.push_back(a.theme.grid);
  for (const int i : order)
    out.palette.push_back(a.theme.series[a.series[i].color_index]);
  if (fill_i >= 0)
    out.palette.push_back(a.theme.series[a.series[order[fill_i]].color_index]);
  return out;
}

}  // namespace geth::ui
