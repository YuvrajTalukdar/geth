#include "ui/png.hpp"

#include <cmath>
#include <cstring>

#include <zlib.h>

#include "ui/glyphs.hpp"

namespace geth::ui {
namespace {

inline std::uint8_t* at(std::vector<std::uint8_t>& px, int W, int x, int y) {
  return &px[(((std::size_t)y * W + x) * 3)];
}

// Alpha-over: blend source color (sr,sg,sb) at opacity f (0..1) onto the
// in-place destination pixel.
inline void over(std::uint8_t* p, int sr, int sg, int sb, double f) {
  if (f <= 0.0) return;
  if (f >= 1.0) {
    p[0] = (std::uint8_t)sr;
    p[1] = (std::uint8_t)sg;
    p[2] = (std::uint8_t)sb;
    return;
  }
  const double i = 1.0 - f;
  p[0] = (std::uint8_t)(sr * f + p[0] * i + 0.5);
  p[1] = (std::uint8_t)(sg * f + p[1] * i + 0.5);
  p[2] = (std::uint8_t)(sb * f + p[2] * i + 0.5);
}

// A soft radial glow: a Gaussian falloff (exp(-2 (d/r)^2)) of `color` at
// opacity `base`/255, composited over the in-place canvas. No blur pass — the
// Gaussian IS the blur (the frame2png.py recipe blurs a hard ellipse; a
// Gaussian centered on the same point looks the same).
void glow(std::vector<std::uint8_t>& px, int W, int H, int cx, int cy,
          int radius, int cr, int cg, int cb, int base) {
  if (radius < 1 || base <= 0) return;
  const int max_d = radius * 2 + 2;  // exp(-2*4) ~ 0.0003 beyond d = 2r
  // d = sqrt(dx^2+dy^2) with |dx|,|dy| <= max_d, so d < 2*max_d — size the
  // table for the diagonal, not the axis.
  std::vector<double> lut((std::size_t)max_d * 2 + 1);
  const double inv = 2.0 / ((double)radius * radius);
  for (int d = 0; d <= max_d; ++d) lut[(std::size_t)d] = std::exp(-inv * d * d);
  const int y0 = std::max(0, cy - max_d), y1 = std::min(H, cy + max_d + 1);
  const int x0 = std::max(0, cx - max_d), x1 = std::min(W, cx + max_d + 1);
  for (int y = y0; y < y1; ++y) {
    const int dy = y - cy;
    for (int x = x0; x < x1; ++x) {
      const int dx = x - cx;
      const int d = (int)std::sqrt((double)dx * dx + (double)dy * dy);
      const double f = lut[(std::size_t)d] * base / 255.0;
      if (f < 0.004) continue;
      over(at(px, W, x, y), cr, cg, cb, f);
    }
  }
}

// The background: a dark vertical gradient (9,11,16) -> (24,28,38) with three
// large soft glows (two accent, one cool), matching frame2png.py. A flat fill
// in solid mode.
void make_background(std::vector<std::uint8_t>& px, int W, int H,
                     const PngOptions& o) {
  if (o.solid) {
    for (std::size_t i = 0; i < px.size(); ++i) {
      px[i * 3] = (std::uint8_t)o.bg_r;
      px[i * 3 + 1] = (std::uint8_t)o.bg_g;
      px[i * 3 + 2] = (std::uint8_t)o.bg_b;
    }
    return;
  }
  for (int y = 0; y < H; ++y) {
    const double t = H > 1 ? (double)y / (H - 1) : 0.0;
    const int r = (int)(9 + (24 - 9) * t + 0.5);
    const int g = (int)(11 + (28 - 11) * t + 0.5);
    const int b = (int)(16 + (38 - 16) * t + 0.5);
    for (int x = 0; x < W; ++x) {
      std::uint8_t* p = at(px, W, x, y);
      p[0] = (std::uint8_t)r;
      p[1] = (std::uint8_t)g;
      p[2] = (std::uint8_t)b;
    }
  }
  const int m = std::min(W, H);
  glow(px, W, H, (int)(W * 0.18), (int)(H * 0.22), (int)(m * 0.55), o.accent_r,
       o.accent_g, o.accent_b, 46);
  glow(px, W, H, (int)(W * 0.85), (int)(H * 0.80), (int)(m * 0.70), o.accent_r,
       o.accent_g, o.accent_b, 34);
  glow(px, W, H, (int)(W * 0.60), (int)(H * 0.05), (int)(m * 0.40), 90, 110,
       160, 26);
}

// The text layer: one embedded glyph per non-space cell, tinted with the cell
// foreground and blended over the canvas at the cell's (off_x, off_y).
void draw_glyphs(std::vector<std::uint8_t>& px, int W, int H,
                 const std::vector<PngCell>& cells, int cols, int rows) {
  for (int y = 0; y < rows; ++y) {
    for (int x = 0; x < cols; ++x) {
      const PngCell& c = cells[(std::size_t)y * cols + x];
      if (c.cp == 32) continue;  // space: blank
      const std::uint8_t* g = glyph_coverage(c.cp, c.bold);
      if (g == nullptr) continue;  // missing glyph: blank (never a crash)
      const int ox = x * kGlyphCw + kGlyphOffX;
      const int oy = y * kGlyphCh + kGlyphOffY;
      for (int gy = 0; gy < kGlyphCh; ++gy) {
        for (int gx = 0; gx < kGlyphCw; ++gx) {
          const int a = g[(std::size_t)gy * kGlyphCw + gx];
          if (a == 0) continue;
          const int X = ox + gx, Y = oy + gy;
          if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
          over(at(px, W, X, Y), c.r, c.g, c.b, a / 255.0);
        }
      }
    }
  }
}

// The pixel rasters, alpha-composited at their graph rects (pixels mode).
void draw_images(std::vector<std::uint8_t>& px, int W, int H,
                 const std::vector<PngImage>& images) {
  for (const PngImage& im : images) {
    if (im.rgba == nullptr || im.w < 1 || im.h < 1) continue;
    const int ox = im.x * kGlyphCw, oy = im.y * kGlyphCh;
    for (int y = 0; y < im.h; ++y) {
      for (int x = 0; x < im.w; ++x) {
        const int X = ox + x, Y = oy + y;
        if (X < 0 || Y < 0 || X >= W || Y >= H) continue;
        const std::uint8_t* s = im.rgba + ((std::size_t)y * im.w + x) * 4;
        if (s[3] == 0) continue;
        over(at(px, W, X, Y), s[0], s[1], s[2], s[3] / 255.0);
      }
    }
  }
}

// -- PNG encoding -------------------------------------------------------------

void put_be32(std::string& s, std::uint32_t v) {
  s.push_back((char)(v >> 24));
  s.push_back((char)(v >> 16));
  s.push_back((char)(v >> 8));
  s.push_back((char)v);
}

void write_chunk(std::string& out, const char type[4],
                 const std::vector<std::uint8_t>& data) {
  put_be32(out, (std::uint32_t)data.size());
  out.append(type, 4);
  out.append((const char*)data.data(), data.size());
  std::uint32_t crc = (std::uint32_t)crc32(0L, (const Bytef*)type, 4);
  crc = (std::uint32_t)crc32(crc, (const Bytef*)data.data(),
                             (uInt)data.size());
  put_be32(out, crc);
}

bool encode_png(const std::vector<std::uint8_t>& px, int W, int H,
                std::string& out) {
  // Raw scanlines: a filter byte (0 = None) per row, then the RGB row.
  std::vector<std::uint8_t> raw(((std::size_t)H) * (1 + (std::size_t)W * 3));
  for (int y = 0; y < H; ++y) {
    std::uint8_t* row = &raw[(std::size_t)y * (1 + (std::size_t)W * 3)];
    row[0] = 0;
    std::memcpy(row + 1, &px[(std::size_t)y * W * 3], (std::size_t)W * 3);
  }
  std::vector<std::uint8_t> comp(compressBound((uLong)raw.size()));
  uLongf comp_len = comp.size();
  if (compress2((Bytef*)comp.data(), &comp_len, (const Bytef*)raw.data(),
                (uLong)raw.size(), 6) != Z_OK)
    return false;
  comp.resize(comp_len);

  out.clear();
  static const std::uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n',
                                       0x1a, '\n'};
  out.append((const char*)kSig, 8);
  // IHDR (13 bytes): width, height, bit depth, color type, compression,
  // filter, interlace.
  std::vector<std::uint8_t> ihdr(13, 0);
  auto be32 = [&](std::size_t off, std::uint32_t v) {
    ihdr[off] = (std::uint8_t)(v >> 24);
    ihdr[off + 1] = (std::uint8_t)(v >> 16);
    ihdr[off + 2] = (std::uint8_t)(v >> 8);
    ihdr[off + 3] = (std::uint8_t)v;
  };
  be32(0, (std::uint32_t)W);
  be32(4, (std::uint32_t)H);
  ihdr[8] = 8;   // bit depth
  ihdr[9] = 2;   // color type: truecolor (RGB)
  ihdr[10] = 0;  // compression
  ihdr[11] = 0;  // filter
  ihdr[12] = 0;  // interlace
  write_chunk(out, "IHDR", ihdr);
  write_chunk(out, "IDAT", comp);
  write_chunk(out, "IEND", {});
  return true;
}

}  // namespace

bool render_png(const std::vector<PngCell>& cells, int cols, int rows,
                const std::vector<PngImage>& images, const PngOptions& opts,
                std::string& out) {
  if (cols < 1 || rows < 1) return false;
  const int W = cols * kGlyphCw, H = rows * kGlyphCh;
  std::vector<std::uint8_t> px((std::size_t)W * H * 3);
  make_background(px, W, H, opts);
  draw_glyphs(px, W, H, cells, cols, rows);
  draw_images(px, W, H, images);
  return encode_png(px, W, H, out);
}

}  // namespace geth::ui
