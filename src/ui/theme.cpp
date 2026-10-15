#include "ui/theme.hpp"

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
    const int c = i - 16;
    put(i, static_cast<std::uint8_t>(levels[(c / 36) / 6]),
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
