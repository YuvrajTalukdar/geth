// Sixel-encoder tests (v1.1.0 Part A, revised for the classic DEC form):
// the stream must be pure classic grammar — `ESC P 9;1q` (no space before
// the 'q': an intermediate byte makes tmux/Konsole drop the whole DCS), the
// "1;1;W;H6" size, `#c;2;r;g;b` declarations (0-100), 6-bit column masks
// (bytes 0x3f-0x7e = 0x3f + the 6-bit mask), `!n` repeats, `#c` switches,
// `-` row advances — because tmux 3.8's parser (image-sixel.c
// sixel_parse) drops the WHOLE image on any other byte (digits, spaces,
// '=' included). decode_sixel mirrors that parser: it
// reports ok=false on exactly the bytes tmux would reject, and reconstructs
// the painted grid (unpainted = -1 = register 0 = the bg).
#include <doctest.h>

#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "ui/graph_backends.hpp"
#include "ui/theme.hpp"

using namespace geth::ui;

namespace {

struct SixelDecode {
  bool dcs = false;   // an ESC P ... q ... ESC\ envelope was found
  bool ok = false;    // the payload parsed as pure classic grammar
  int p2 = 0;         // the second DCS param (the transparency flag)
  bool has_size = false;
  int w = 0, h6 = 0;
  std::vector<std::array<int, 3>> colors;  // register -> r,g,b (0-100)
  std::vector<std::vector<int>> grid;  // rows x cols, register index, -1 = unpainted
};

// Mirrors tmux's sixel_parse: '"' size (4 numbers, the last two are W,H),
// '#' colour (c or c;t;c1;c2;c3), '!' repeat (n + one data byte), '-' row
// advance (dx=0, dy+=6), '$' carriage return (dx=0), data bytes 0x3f-0x7e
// (a 6-bit column mask in the current register, then dx++). Control bytes
// (< 0x20) are ignored exactly as tmux ignores them; ANY other byte
// (digits, '=', spaces, letters outside the data range) fails the whole
// image.
SixelDecode decode_sixel(const std::string& s) {
  SixelDecode d;
  const std::size_t i0 = s.find("\x1bP");
  if (i0 == std::string::npos) return d;
  std::size_t p = i0 + 2;
  // The DCS params up to the 'q' final byte.
  std::vector<std::string> params;
  std::string cp;
  while (p < s.size() && s[p] != 'q') {
    const unsigned char uc = s[p];
    // tmux's DCS state machine: an intermediate byte (0x20-0x2f — e.g. a
    // SPACE between the params and the 'q' final byte) makes dispatch
    // (buf[0]=='q' && interm_len==0) fail and the WHOLE image is silently
    // dropped; 0x3a/0x3c-0x3f/0x7f+ abort or ignore the sequence likewise.
    if ((uc >= 0x20 && uc <= 0x2f) || uc == 0x3a ||
        (uc >= 0x3c && uc <= 0x3f) || uc >= 0x7f)
      return d;
    if (s[p] == ';') {
      params.push_back(cp);
      cp.clear();
    } else {
      cp += s[p];
    }
    ++p;
  }
  if (p >= s.size() || s[p] != 'q') return d;
  params.push_back(cp);
  if (params.size() >= 2 && !params[1].empty()) d.p2 = std::atoi(params[1].c_str());
  ++p;  // the 'q'
  const std::size_t end = s.find("\x1b\\", p);
  if (end == std::string::npos) return d;
  d.dcs = true;
  const std::size_t n = end - p;

  auto ensure = [&](int row, int col) {
    if ((int)d.grid.size() <= row) d.grid.resize(row + 1);
    auto& r = d.grid[row];
    if ((int)r.size() <= col) r.resize(col + 1, -1);
  };
  int cur = -1;  // the current register (-1 = the background, tmux dc = 0)
  int dx = 0, dy = 0;
  auto paint = [&](int mask) {
    for (int i = 0; i < 6; ++i)
      if (mask & (1 << i)) {
        ensure(dy + i, dx);
        d.grid[dy + i][dx] = cur;
      }
    ++dx;
  };
  for (std::size_t i = 0; i < n;) {
    const char ch = s[p + i++];
    switch (ch) {
      case '"': {
        // The argument spans digits and ';' (tmux scans to the first other
        // byte), then exactly 4 numbers: a;b;W;H.
        std::size_t j = i;
        while (j < n &&
               (std::isdigit((unsigned char)s[p + j]) || s[p + j] == ';'))
          ++j;
        int vals[4] = {-1, -1, -1, -1};
        int nv = 0, num = 0;
        bool in_num = false;
        for (std::size_t k = i; k < j; ++k) {
          const char c2 = s[p + k];
          if (std::isdigit((unsigned char)c2)) {
            num = num * 10 + (c2 - '0');
            in_num = true;
          } else if (c2 == ';' && in_num && nv < 4) {
            vals[nv++] = num;
            num = 0;
            in_num = false;
          }
        }
        if (in_num && nv < 4) vals[nv++] = num;
        if (nv != 4 || vals[2] <= 0 || vals[3] <= 0 || vals[2] > 10000 ||
            vals[3] > 10000) {
          d.ok = false;
          return d;
        }
        d.w = vals[2];
        d.h6 = vals[3];
        d.has_size = true;
        i = j;
        break;
      }
      case '#': {
        // c or c;t;c1;c2;c3 — the argument spans digits and ';'.
        std::size_t j = i;
        while (j < n &&
               (std::isdigit((unsigned char)s[p + j]) || s[p + j] == ';'))
          ++j;
        int vals[5] = {-1, -1, -1, -1, -1};
        int nv = 0, num = 0;
        bool in_num = false;
        for (std::size_t k = i; k < j; ++k) {
          const char c2 = s[p + k];
          if (std::isdigit((unsigned char)c2)) {
            num = num * 10 + (c2 - '0');
            in_num = true;
          } else if (c2 == ';' && in_num && nv < 5) {
            vals[nv++] = num;
            num = 0;
            in_num = false;
          }
        }
        if (in_num && nv < 5) vals[nv++] = num;
        if (nv == 0) {
          // tmux: strtoul on the empty arg selects colour 0 without
          // consuming the following byte.
          cur = 0;
        } else if (nv == 1) {
          if (vals[0] < 0 || vals[0] > 1024) {
            d.ok = false;
            return d;
          }
          cur = vals[0];
        } else if (nv == 5) {
          if (vals[0] < 0 || vals[0] > 1024 || (vals[1] != 1 && vals[1] != 2) ||
              vals[2] < 0 || vals[3] < 0 || vals[4] < 0 ||
              (vals[1] == 2 && (vals[2] > 100 || vals[3] > 100 ||
                                vals[4] > 100))) {
            d.ok = false;
            return d;
          }
          while ((int)d.colors.size() <= vals[0]) d.colors.push_back({0, 0, 0});
          d.colors[vals[0]] = {vals[2], vals[3], vals[4]};
          cur = vals[0];
        } else {
          d.ok = false;
          return d;
        }
        i = j;
        break;
      }
      case '!': {
        // n + one data byte.
        int num = 0;
        bool in_num = false;
        std::size_t j = i;
        while (j < n && std::isdigit((unsigned char)s[p + j])) {
          num = num * 10 + (s[p + j] - '0');
          in_num = true;
          if (j - i >= 9) {
            d.ok = false;  // longer than any possible count (overflow guard)
            return d;
          }
          ++j;
        }
        if (!in_num || j >= n || num < 1 || num > 10000) {
          d.ok = false;
          return d;
        }
        const int mask = (s[p + j] - 0x3f) & 0x3f;
        for (int k = 0; k < num; ++k) paint(mask);
        i = j + 1;
        break;
      }
      case '-':
        dx = 0;
        dy += 6;
        break;
      case '$':
        dx = 0;
        break;
      default:
        if (ch < 0x20) break;  // tmux ignores control bytes
        if (ch < 0x3f || ch > 0x7e) {
          d.ok = false;  // digits, '=', spaces, ...: tmux drops the image
          return d;
        }
        paint((unsigned)(ch - 0x3f));
    }
  }
  // tmux's sixel_parse: an image without a size attribute (or with x/y == 0)
  // is dropped.
  d.ok = d.has_size;
  return d;
}

// The encoder's fill-gradient step formula (mirrored so a test pixel can be
// set to the EXACT step color).
Rgb step_color(int s2, const Rgb& fc, const Rgb& bg) {
  const double a = 0.35 + (0.05 - 0.35) * (s2 - 1) / 23.0;
  return rgb((int)std::lround(fc.r * a + bg.r * (1.0 - a)),
             (int)std::lround(fc.g * a + bg.g * (1.0 - a)),
             (int)std::lround(fc.b * a + bg.b * (1.0 - a)));
}

}  // namespace

TEST_CASE("sixel: classic structure, size, and decode-back") {
  const int w = 10, h = 10;
  const Rgb bg = rgb(20, 30, 40);
  // Opaque bg everywhere, a 2x2 red square at x4-5, y4-5.
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
  CHECK(d.dcs);
  CHECK(d.ok);  // pure classic grammar (tmux would parse it)
  CHECK_EQ(d.p2, 1);  // the transparency flag in DCS param index 1
  CHECK(d.has_size);
  CHECK_EQ(d.w, w);
  const int h6 = ((h + 5) / 6) * 6;  // 12
  CHECK_EQ(d.h6, h6);
  CHECK_LE((int)d.colors.size(), 256);
  CHECK_GE((int)d.colors.size(), 3);  // bg + grid + red (+ 24 fill steps)

  // The grid is sparse: never-painted rows/cells simply don't exist.
  auto cell = [&](int y, int x) -> int {
    if (y < 0 || y >= (int)d.grid.size()) return -1;
    if (x < 0 || x >= (int)d.grid[y].size()) return -1;
    return d.grid[y][x];
  };
  // The square decodes to ONE consistent register, declared as (100,0,0).
  const int red_idx = cell(4, 4);
  CHECK(red_idx >= 0);
  CHECK_EQ(d.colors[red_idx][0], 100);
  CHECK_EQ(d.colors[red_idx][1], 0);
  CHECK_EQ(d.colors[red_idx][2], 0);
  CHECK_EQ(cell(4, 5), red_idx);
  CHECK_EQ(cell(5, 4), red_idx);
  CHECK_EQ(cell(5, 5), red_idx);
  // Every other painted cell is inside the square: all other cells are
  // unpainted (-1) — the bg is the UNPAINTED image (register 0), not an
  // explicit color.
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const bool in_sq = (x >= 4 && x <= 5 && y >= 4 && y <= 5);
      CHECK_EQ(cell(y, x), in_sq ? red_idx : -1);
    }
  // The padded rows (y >= h) are unpainted too.
  for (int y = h; y < h6; ++y) CHECK_EQ(cell(y, 0), -1);
}

TEST_CASE("sixel: alpha-0 pixels and opaque bg stay unpainted") {
  const int w = 6, h = 6;
  const Rgb bg = rgb(10, 12, 14);
  // A column of alpha-0 (transparent) pixels and one opaque bg pixel.
  std::vector<std::uint8_t> rgba((std::size_t)w * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      std::uint8_t* px = rgba.data() + ((std::size_t)y * w + x) * 4;
      px[0] = bg.r;
      px[1] = bg.g;
      px[2] = bg.b;
      px[3] = (x == 0) ? 0 : 255;
    }
  const std::string s = sixel_encode(w, h, rgba.data(), bg,
                                     {rgb(255, 0, 0)});
  const SixelDecode d = decode_sixel(s);
  CHECK(d.ok);
  // Nothing is painted: the grid is empty (cell() = -1 on absence).
  auto cell = [&](int y, int x) -> int {
    if (y < 0 || y >= (int)d.grid.size()) return -1;
    if (x < 0 || x >= (int)d.grid[y].size()) return -1;
    return d.grid[y][x];
  };
  for (int y = 0; y < h; ++y) {
    CHECK_EQ(cell(y, 0), -1);  // alpha-0
    CHECK_EQ(cell(y, 1), -1);  // opaque bg color
  }
}

TEST_CASE("sixel: a series column beats a fill column (exact mask)") {
  const int w = 1, h = 6;
  const Rgb bg = rgb(20, 30, 40);
  const Rgb series = rgb(255, 0, 0);
  // One column: the grid at row 0, a fill step at rows 1-5.
  const Rgb step = step_color(12, series, bg);
  std::vector<std::uint8_t> rgba((std::size_t)w * h * 4);
  for (int y = 0; y < h; ++y) {
    std::uint8_t* px = rgba.data() + y * 4;
    const Rgb c = (y == 0) ? rgb(90, 90, 90) : step;
    px[0] = c.r;
    px[1] = c.g;
    px[2] = c.b;
    px[3] = 255;
  }
  // palette[0] = the grid, palette[1] = the series (the fill color).
  const std::string s = sixel_encode(w, h, rgba.data(), bg,
                                     {rgb(90, 90, 90), series});
  const SixelDecode d = decode_sixel(s);
  CHECK(d.ok);
  // The grid (priority 1) beats the fill step (priority 2) — and the mask is
  // the grid's OWN rows only (no fill tail, no fill hole below the line).
  auto cell = [&](int y, int x) -> int {
    if (y < 0 || y >= (int)d.grid.size()) return -1;
    if (x < 0 || x >= (int)d.grid[y].size()) return -1;
    return d.grid[y][x];
  };
  const int grid_idx = cell(0, 0);
  CHECK(grid_idx >= 0);
  for (int y = 1; y < 6; ++y) CHECK_EQ(cell(y, 0), -1);
}

TEST_CASE("sixel: fill-step columns merge (no holes in the fill)") {
  const int w = 1, h = 6;
  const Rgb bg = rgb(20, 30, 40);
  const Rgb series = rgb(255, 0, 0);
  // One column: two ADJACENT fill steps (the gradient moves a step or two
  // inside a 6 px band) — rows 0-2 on step 12, rows 3-5 on step 13.
  const Rgb s12 = step_color(12, series, bg);
  const Rgb s13 = step_color(13, series, bg);
  std::vector<std::uint8_t> rgba((std::size_t)w * h * 4);
  for (int y = 0; y < h; ++y) {
    std::uint8_t* px = rgba.data() + y * 4;
    const Rgb c = (y < 3) ? s12 : s13;
    px[0] = c.r;
    px[1] = c.g;
    px[2] = c.b;
    px[3] = 255;
  }
  const std::string s = sixel_encode(w, h, rgba.data(), bg, {series});
  const SixelDecode d = decode_sixel(s);
  CHECK(d.ok);
  // One register for the whole column (the winning step, 5 rows), merged
  // over all 6 rows.
  auto cell = [&](int y, int x) -> int {
    if (y < 0 || y >= (int)d.grid.size()) return -1;
    if (x < 0 || x >= (int)d.grid[y].size()) return -1;
    return d.grid[y][x];
  };
  const int c0 = cell(0, 0);
  CHECK(c0 >= 0);
  for (int y = 1; y < 6; ++y) CHECK_EQ(cell(y, 0), c0);
}

TEST_CASE("sixel: an all-bg image is one empty-column run") {
  const int w = 20, h = 6;
  const Rgb bg = rgb(11, 15, 20);
  std::vector<std::uint8_t> rgba((std::size_t)w * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      std::uint8_t* px = rgba.data() + ((std::size_t)y * w + x) * 4;
      px[0] = bg.r;
      px[1] = bg.g;
      px[2] = bg.b;
      px[3] = 255;
    }
  const std::string s = sixel_encode(w, h, rgba.data(), bg, {rgb(0, 255, 0)});
  const SixelDecode d = decode_sixel(s);
  CHECK(d.ok);
  CHECK_EQ(d.w, w);
  // 20 identical empty columns compress to one !n run.
  CHECK(s.find("!20") != std::string::npos);
  auto cell = [&](int y, int x) -> int {
    if (y < 0 || y >= (int)d.grid.size()) return -1;
    if (x < 0 || x >= (int)d.grid[y].size()) return -1;
    return d.grid[y][x];
  };
  for (int y = 0; y < h; ++y) CHECK_EQ(cell(y, 0), -1);
}

TEST_CASE("sixel: the tmux parser rejects non-classic bytes (decoder mirror)") {
  // The old named-param format: 'P' parses as a data byte, '1' (< 0x3f)
  // fails — tmux drops the whole image.
  CHECK_FALSE(decode_sixel("\x1bP9;1q\"1;1;4;6P1=0\x1b\\").ok);
  // A stray space in the PAYLOAD: dropped (control bytes <0x20 are ignored,
  // but 0x20 is not a control byte).
  CHECK_FALSE(decode_sixel("\x1bP9;1q\"1;1;4;6 #0;2;10;10;10\x1b\\").ok);
  // A stray '=': dropped.
  CHECK_FALSE(decode_sixel("\x1bP9;1q\"1;1;4;6#0=10\x1b\\").ok);
  // No size attribute at all: tmux's sixel_parse requires it (x/y == 0).
  CHECK_FALSE(decode_sixel("\x1bP9;1q#0;2;10;10;10\x1b\\").ok);
  // A space between the DCS params and the 'q' final byte is an
  // INTERMEDIATE byte: dispatch (interm_len==0) fails and the whole image
  // is dropped (the historical "9;1 q" bug — no space, ever).
  CHECK_FALSE(decode_sixel("\x1bP9;1 q\"1;1;4;6#0;2;10;10;10\x1b\\").ok);
  // A bare data byte + row advance is valid classic grammar.
  CHECK(decode_sixel("\x1bP9;1q\"1;1;4;6#0;2;10;10;10@-\x1b\\").ok);
}

TEST_CASE("sixel: the palette is capped at 256 registers") {
  const int w = 6, h = 6;
  const Rgb bg = rgb(11, 15, 20);
  std::vector<std::uint8_t> rgba((std::size_t)w * h * 4);
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      std::uint8_t* px = rgba.data() + ((std::size_t)y * w + x) * 4;
      px[0] = (std::uint8_t)(x * 40);
      px[1] = bg.g;
      px[2] = bg.b;
      px[3] = 255;
    }
  // 300 distinct "important" colors — far over the cap.
  std::vector<Rgb> palette;
  for (int i = 0; i < 300; ++i)
    palette.push_back(rgb((std::uint8_t)(i % 256), (std::uint8_t)(50 + i % 200),
                          (std::uint8_t)(i * 7 % 256)));
  const std::string s = sixel_encode(w, h, rgba.data(), bg, palette);
  const SixelDecode d = decode_sixel(s);
  CHECK(d.ok);
  CHECK_LE((int)d.colors.size(), 256);
}
