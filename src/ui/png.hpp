// PNG writer + frame composer for `geth --render-png`.
//
// Composites, in order: (1) a procedurally generated dark background — a
// vertical gradient plus a few large soft radial glows (the same recipe
// frame2png.py uses, rendered here without the blur pass: each glow is a
// direct Gaussian falloff, which is visually the blurred ellipse); (2) the
// text layer, one embedded glyph per cell tinted with the cell's truecolor
// foreground; (3) the pixel rasters (pixels mode), alpha-composited at their
// graph rects. The result is encoded as an 8-bit RGB PNG (IHDR / IDAT via
// zlib / IEND) with no external PNG dependency.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace geth::ui {

// One text-layer cell. `cp` is the code point (32 = space, left blank); the
// foreground is already resolved to a concrete RGB (the caller substitutes the
// default (215,220,228) for an unset/default color).
struct PngCell {
  std::uint32_t cp = 32;
  std::uint8_t r = 215, g = 220, b = 228;
  bool bold = false;
};

// One pixel raster (a graph in pixels mode), placed at cell rect (x, y) with
// pixel dimensions w x h.
struct PngImage {
  int x = 0, y = 0;
  const std::uint8_t* rgba = nullptr;
  int w = 0, h = 0;
};

struct PngOptions {
  // Flat `bg` fill when solid; otherwise the gradient + glow recipe below.
  bool solid = false;
  int bg_r = 8, bg_g = 10, bg_b = 14;
  // The accent color for the two accent glows (the theme's accent).
  int accent_r = 74, accent_g = 111, accent_b = 165;  // #4a6fa5
};

// Compose the frame and encode it as an 8-bit RGB PNG into `out`. `cells` is
// rows x cols (row-major). Returns false if the zlib compression fails.
bool render_png(const std::vector<PngCell>& cells, int cols, int rows,
                const std::vector<PngImage>& images, const PngOptions& opts,
                std::string& out);

}  // namespace geth::ui
