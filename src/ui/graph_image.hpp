// The panel-graph rasterizer (v1.1.0, Part A): one graph rect as an RGBA8
// image, in the KDE System Monitor look — a soft translucent fill under the
// line, smooth anti-aliased lines, faint gridlines, on a transparent
// background (or the theme's bg in solid mode).
//
// The geometry and data path come from graph_data (the SAME computation the
// text renderer uses), so the image sits exactly where the text layer
// reserved the graph rect, and the line is the same line the braille/area
// chart draws.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "ui/graph_data.hpp"
#include "ui/panel_spec.hpp"
#include "ui/theme.hpp"

namespace geth::ui {

// An RGBA8 image, row-major, top row first, (R,G,B,A) per pixel.
struct RasterImage {
  std::vector<std::uint8_t> rgba;
  int w = 0, h = 0;
  // The colors the image is built from, in draw priority: the gridline
  // color first, the series colors in draw order (non-primary first, the
  // primary last), the fill color last. The sixel encoder quantizes onto
  // exactly these plus its gradient steps; the kitty encoder ignores it.
  std::vector<Rgb> palette;
  bool valid() const {
    return w > 0 && h > 0 && rgba.size() == (std::size_t)w * h * 4;
  }
  std::uint8_t* at(int x, int y) {
    return rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
  }
  const std::uint8_t* at(int x, int y) const {
    return rgba.data() + (static_cast<std::size_t>(y) * w + x) * 4;
  }
};

// Everything raster_panel needs. `raws`/`decs`/`series` are parallel
// (series i's in-window window, its decimation to geom.dot_cols, its spec).
// `geom` is the text renderer's geometry (rect, ticks, vmax) — the y
// mapping value_y_px()/tick_y_px() comes from it, so the image's lines and
// gridlines land on the same pixel rows the text labels describe.
struct RasterArgs {
  const GraphGeom& geom;
  int cell_w = 0, cell_h = 0;
  const Theme& theme;
  bool solid_bg = false;
  const std::vector<RawWindow>& raws;
  const std::vector<Decimated>& decs;
  const std::vector<SeriesSpec>& series;
};

// Rasterizes ONE panel's graph rect: w = Wg*cell_w, h = Hg*cell_h.
// Layers, in order:
//   1. background: transparent (alpha 0) or the theme's bg (solid mode);
//   2. gridlines: 1 px at each tick row (tick_y_px), the theme's grid at
//      60 % alpha — the same rows the text tick labels sit on;
//   3. the fill under the first line with data: a vertical gradient from
//      35 % alpha at the line to 5 % at the image bottom (halved when the
//      panel has 3+ series, so it cannot compete with the lines);
//   4. the lines: coverage-anti-aliased capsules (width max(1.5, cell_h/9))
//      at the series colors, sampled at twice the pixel-column count; the
//      primary (series 0) is drawn last, on top.
// A NaN gap (value_at returns NaN) breaks both the line and the fill — a
// gap is never drawn as a 0-floor.
RasterImage raster_panel(const RasterArgs& a);

}  // namespace geth::ui
