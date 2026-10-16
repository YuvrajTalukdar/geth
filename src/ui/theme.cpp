#include "ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace geth::ui {

bool parse_color_mode(const std::string& s, ColorMode& out) {
  if (s == "truecolor" || s == "24bit") out = ColorMode::TrueColor;
  else if (s == "256") out = ColorMode::Color256;
  else if (s == "16") out = ColorMode::Color16;
  else return false;
  return true;
}

ColorMode detect_color_mode() {
  // Spec: truecolor when COLORTERM is truecolor or 24bit, else 256.
  if (const char* ct = std::getenv("COLORTERM"))
    if (std::strcmp(ct, "truecolor") == 0 || std::strcmp(ct, "24bit") == 0)
      return ColorMode::TrueColor;
  return ColorMode::Color256;
}

Rgb scale(Rgb c, double f) {
  return {static_cast<std::uint8_t>(c.r * f + 0.5),
          static_cast<std::uint8_t>(c.g * f + 0.5),
          static_cast<std::uint8_t>(c.b * f + 0.5)};
}

Rgb derive_fill(Rgb series) { return scale(series, 0.45); }

Rgb mix_white(Rgb c) {
  return {static_cast<std::uint8_t>((c.r + 255) / 2),
          static_cast<std::uint8_t>((c.g + 255) / 2),
          static_cast<std::uint8_t>((c.b + 255) / 2)};
}

double hue_deg(Rgb c) {
  const int mx = std::max(c.r, std::max(c.g, c.b));
  const int mn = std::min(c.r, std::min(c.g, c.b));
  const int d = mx - mn;
  if (d == 0) return -1.0;
  double h;
  if (mx == (int)c.r)
    h = 60.0 * std::fmod((double)(c.g - c.b) / d, 6.0);
  else if (mx == (int)c.g)
    h = 60.0 * (double)(c.b - c.r) / d + 120.0;
  else
    h = 60.0 * (double)(c.r - c.g) / d + 240.0;
  if (h < 0.0) h += 360.0;
  return h;
}

namespace {

// ANSI-16 in FTXUI's Palette16 order (0-7 dark, 8-15 bright), the standard
// xterm RGB values.
constexpr Rgb kAnsi16[16] = {
    {0, 0, 0},       {205, 0, 0},    {0, 205, 0},  {205, 205, 0},
    {0, 0, 238},     {205, 0, 205},  {0, 205, 205}, {229, 229, 229},
    {127, 127, 127}, {255, 0, 0},    {0, 255, 0},   {255, 255, 0},
    {0, 0, 255},     {255, 0, 255},  {0, 255, 255}, {255, 255, 255}};

std::uint8_t kPalette256[256][3];  // filled in nearest256() on first use

void build_palette256() {
  static bool done = false;
  if (done) return;
  // C arrays are not assignable, so write the triple element-wise.
  auto put = [](int i, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    kPalette256[i][0] = r;
    kPalette256[i][1] = g;
    kPalette256[i][2] = b;
  };
  for (int i = 0; i < 16; ++i) put(i, kAnsi16[i].r, kAnsi16[i].g, kAnsi16[i].b);
  const int levels[6] = {0, 95, 135, 175, 215, 255};
  for (int i = 16; i < 232; ++i) {
    // c = r * 36 + g * 6 + b, so the component indices are c/36, (c/6) % 6,
    // c % 6. (An older draft wrote (c/36)/6 for the red, which is always 0:
    // every cube color then had r == 0 and red-rich colors like the pink
    // series quantized onto grey ramp colors.)
    const int c = i - 16;
    put(i, static_cast<std::uint8_t>(levels[c / 36]),
        static_cast<std::uint8_t>(levels[(c / 6) % 6]),
        static_cast<std::uint8_t>(levels[c % 6]));
  }
  for (int i = 232; i < 256; ++i) {
    const int g = 8 + (i - 232) * 10;
    put(i, static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(g),
        static_cast<std::uint8_t>(g));
  }
  done = true;
}

long long sqdist(Rgb a, Rgb b) {
  const long long dr = a.r - b.r, dg = a.g - b.g, db = a.b - b.b;
  return dr * dr + dg * dg + db * db;
}

}  // namespace

std::uint8_t nearest256(Rgb c) {
  build_palette256();
  int best = 0;
  long long best_d = -1;
  for (int i = 0; i < 256; ++i) {
    const Rgb p{kPalette256[i][0], kPalette256[i][1], kPalette256[i][2]};
    const long long d = sqdist(c, p);
    if (best_d < 0 || d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return static_cast<std::uint8_t>(best);
}

std::uint8_t nearest16(Rgb c) {
  int best = 0;
  long long best_d = -1;
  for (int i = 0; i < 16; ++i) {
    const long long d = sqdist(c, kAnsi16[i]);
    if (best_d < 0 || d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return static_cast<std::uint8_t>(best);
}

ftxui::Color to_ftxui(Rgb c, ColorMode mode) {
  switch (mode) {
    case ColorMode::TrueColor: return ftxui::Color::RGB(c.r, c.g, c.b);
    case ColorMode::Color256: return ftxui::Color(ftxui::Color::Palette256(nearest256(c)));
    case ColorMode::Color16: return ftxui::Color(ftxui::Color::Palette16(nearest16(c)));
  }
  return ftxui::Color::RGB(c.r, c.g, c.b);
}

ftxui::Color fill_color(Rgb series, ColorMode mode) {
  const Rgb target = derive_fill(series);
  if (mode == ColorMode::TrueColor) return to_ftxui(target, mode);
  build_palette256();
  const bool m256 = (mode == ColorMode::Color256);
  const int n = m256 ? 256 : 16;
  const double hs = hue_deg(series);
  // A hue-having series may only match palette colors that have a hue too,
  // within 30 degrees: an achromatic candidate (grey ramp) is always "close"
  // in plain RGB distance and would swallow e.g. the violet fill.
  int best = -1;
  long long best_d = -1;
  for (int i = 0; i < n; ++i) {
    const Rgb p = m256
                      ? Rgb{kPalette256[i][0], kPalette256[i][1], kPalette256[i][2]}
                      : kAnsi16[i];
    const double hp = hue_deg(p);
    if (hs >= 0.0) {
      if (hp < 0.0) continue;  // grey: never a shade of a colored series
      double dh = std::fabs(hs - hp);
      if (dh > 180.0) dh = 360.0 - dh;
      if (dh > 30.0) continue;  // outside the series' hue family
    }
    const long long d = sqdist(target, p);
    if (best < 0 || d < best_d) {
      best_d = d;
      best = i;
    }
  }
  if (hs >= 0.0 && best < 0) {
    // No palette color in the family (impossible for the shipped palette,
    // kept for safety): fall back to the plain nearest color.
    best_d = -1;
    for (int i = 0; i < n; ++i) {
      const Rgb p = m256
                        ? Rgb{kPalette256[i][0], kPalette256[i][1], kPalette256[i][2]}
                        : kAnsi16[i];
      const long long d = sqdist(target, p);
      if (best_d < 0 || d < best_d) {
        best_d = d;
        best = i;
      }
    }
  }
  return m256
             ? ftxui::Color(ftxui::Color::Palette256(static_cast<std::uint8_t>(best)))
             : ftxui::Color(ftxui::Color::Palette16(static_cast<std::uint8_t>(best)));
}

Theme Theme::cyberpunk_blue() {
  Theme t;
  // Starting values (spec); tuned by eye.
  t.accent = rgb(0x00, 0xE5, 0xFF);   // #00E5FF
  t.title = rgb(0x00, 0xE5, 0xFF);    // #00E5FF
  t.text = rgb(0x9E, 0xEF, 0xFF);     // #9EEFFF
  t.label_dim = rgb(0x4F, 0xA3, 0xB3);  // #4FA3B3
  t.border = rgb(0x0F, 0x5A, 0x66);    // #0F5A66
  t.grid = rgb(0x0A, 0x3A, 0x42);      // #0A3A42
  t.axis_text = rgb(0x3E, 0x8C, 0x99);  // #3E8C99
  t.warn = rgb(0xFF, 0x3B, 0x5C);      // #FF3B5C
  // New order, for maximum contrast between the first three series
  // (cyan / green / pink). Phase 5 makes this tunable.
  t.series[0] = rgb(0x00, 0xE5, 0xFF);  // #00E5FF cyan
  t.series[1] = rgb(0x39, 0xFF, 0x88);  // #39FF88 green
  t.series[2] = rgb(0xFF, 0x4F, 0xD8);  // #FF4FD8 pink
  t.series[3] = rgb(0xFF, 0xD2, 0x3F);  // #FFD23F amber
  t.series[4] = rgb(0x4D, 0xA3, 0xFF);  // #4DA3FF blue
  t.series[5] = rgb(0xC7, 0x7D, 0xFF);  // #C77DFF violet
  return t;
}

}  // namespace geth::ui
