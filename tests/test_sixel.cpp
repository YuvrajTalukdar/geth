// Sixel-encoder tests (v1.1.0 Part A): the libsixel dialect Konsole speaks —
// P2=1 transparent background, a small palette, R-run rows, height padded to a
// multiple of 6. These decode the emitted runs back into a w x h6 color grid
// (tracking the protocol's "current color = last S command" rule) and check the
// structure and the placement of the colors.
#include <doctest.h>

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "ui/graph_backends.hpp"
#include "ui/theme.hpp"

using namespace geth::ui;

namespace {

struct SixelDecode {
  bool p2_1 = false;
  int p4 = 0, p5 = 0;
  int palette = 0;
  std::vector<std::vector<int>> grid;  // h6 rows x p5 cols, palette indices
};

// Decodes a sixel stream into a color grid. The current color starts at the
// LAST palette entry (the protocol rule) — so a stream that omits the initial
// S0= for a bg first pixel mis-decodes it to the last palette color, which is
// exactly the bug this guard is meant to catch.
SixelDecode decode_sixel(const std::string& s) {
  SixelDecode out;
  const std::size_t end_attr = s.find('R');
  if (end_attr == std::string::npos) return out;
  const std::string header = s.substr(0, end_attr);
  out.p2_1 = header.find("P2=1") != std::string::npos;
  auto parse_num = [&](const char* key) {
    const std::size_t p = header.find(key);
    if (p == std::string::npos) return 0;
    std::size_t q = p + std::char_traits<char>::length(key);
    int v = 0;
    while (q < header.size() && std::isdigit(static_cast<unsigned char>(header[q])))
      v = v * 10 + (header[q++] - '0');
    return v;
  };
  out.p4 = parse_num("P4=");
  out.p5 = parse_num("P5=");
  {  // count the palette entries (S<digits>= in the header)
    std::size_t p = 0;
    while ((p = header.find('S', p)) != std::string::npos) {
      std::size_t q = p + 1;
      while (q < header.size() &&
             std::isdigit(static_cast<unsigned char>(header[q])))
        ++q;
      if (q < header.size() && header[q] == '=') {
        ++out.palette;
        p = q;
      } else {
        ++p;
      }
    }
  }
  const std::size_t end =
      s.find(std::string(1, static_cast<char>(0x1b)) + "\\");
  std::size_t pos = end_attr + 1;
  int cur = out.palette - 1;  // the protocol's starting current color
  while (pos < end && out.grid.size() < (std::size_t)out.p4) {
    std::vector<int> row;
    while (pos < end) {
      if (s[pos] == 'R') {
        ++pos;
        break;
      }
      if (s[pos] == 'S') {
        std::size_t q = pos + 1;
        int c = 0;
        while (q < end && std::isdigit(static_cast<unsigned char>(s[q])))
          c = c * 10 + (s[q++] - '0');
        if (q < end && s[q] == '=') {
          cur = c;
          pos = q + 1;
        } else {
          ++pos;
        }
        continue;
      }
      if (std::isdigit(static_cast<unsigned char>(s[pos]))) {
        int len = 0;
        while (pos < end && std::isdigit(static_cast<unsigned char>(s[pos])))
          len = len * 10 + (s[pos++] - '0');
        for (int i = 0; i < len; ++i) row.push_back(cur);
        continue;
      }
      ++pos;  // skip an unexpected byte
    }
    out.grid.push_back(std::move(row));
  }
  return out;
}

}  // namespace

TEST_CASE("sixel: structure, palette cap, x6 height, and color placement") {
  const int w = 10, h = 10;
  const Rgb bg = rgb(20, 30, 40);
  // An OPAQUE image: the background is the bg color (so the first pixel is
  // palette index 0 — the case that exercises the initial current color), with
  // a small red square in the middle.
  std::vector<std::uint8_t> rgba((std::size_t)w * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      std::uint8_t* px = rgba.data() + ((std::size_t)y * w + x) * 4;
      const bool red = (x >= 4 && x <= 5 && y >= 4 && y <= 5);
      px[0] = red ? 255 : bg.r;
      px[1] = red ? 0 : bg.g;
      px[2] = red ? 0 : bg.b;
      px[3] = 255;
    }
  const std::vector<Rgb> palette = {rgb(90, 90, 90), rgb(255, 0, 0)};
  const std::string s = sixel_encode(w, h, rgba.data(), bg, palette);

  const SixelDecode d = decode_sixel(s);
  CHECK(d.p2_1);  // transparent background flag
  CHECK_EQ(d.p5, w);
  const int h6 = ((h + 5) / 6) * 6;  // 12 for h = 10
  CHECK_EQ(d.p4, h6);
  CHECK_EQ(h6 % 6, 0);
  CHECK_LE(d.palette, 256);
  CHECK(d.palette >= 2);  // at least bg + the transparent slot

  // The grid decodes to h6 rows, each exactly w wide.
  REQUIRE_EQ(d.grid.size(), (std::size_t)h6);
  for (const auto& row : d.grid) CHECK_EQ((int)row.size(), w);

  // The first pixel is the opaque bg -> palette index 0. (A stream that skips
  // the initial S0= would decode this to the last palette color instead.)
  CHECK_EQ(d.grid[0][0], 0);
  // The red square maps to one consistent non-bg, non-transparent color.
  const int red_idx = d.grid[4][4];
  CHECK(red_idx != 0);
  CHECK(red_idx != 1);
  CHECK_EQ(d.grid[4][5], red_idx);
  CHECK_EQ(d.grid[5][5], red_idx);
  // A bg pixel next to the square is still index 0.
  CHECK_EQ(d.grid[4][3], 0);
  // The padded rows (y >= h) are transparent (index 1).
  CHECK_EQ(d.grid[h][0], 1);  // row h (the first padded row)
  CHECK_EQ(d.grid[h6 - 1][w - 1], 1);
}
