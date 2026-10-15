// Color theme for the UI. Phase 2 ships one theme (Cyberpunk Blue); the
// structure (a plain struct of RGB fields, every UI color read from it) is
// what Phase 5's multiple themes plug into.
//
// Colors are stored as 24-bit RGB and converted to the terminal's color
// support at draw time:
//   truecolor  -> 38;2;R;G;B (when COLORTERM is truecolor/24bit)
//   256        -> nearest xterm-256 color
//   16         -> nearest ANSI-16 color
// `--color=truecolor|256|16` overrides the auto-detection.
//
// GETH never sets a background color anywhere (the user's terminal uses
// background blur) — the Theme has no background fields on purpose.
#pragma once

#include <cstdint>

#include "ftxui/screen/color.hpp"

namespace geth::ui {

enum class ColorMode { TrueColor, Color256, Color16 };

// Parses "truecolor"|"24bit"|"256"|"16"; false on anything else.
bool parse_color_mode(const std::string& s, ColorMode& out);

// Auto-detect: truecolor when COLORTERM is truecolor or 24bit, else 256.
ColorMode detect_color_mode();

struct Rgb {
  std::uint8_t r = 0, g = 0, b = 0;
};

constexpr Rgb rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) { return {r, g, b}; }

// Component-wise darken. fill(i) = series[i] × 0.45 (spec).
Rgb scale(Rgb c, double f);

// Nearest xterm-256 palette index for c.
std::uint8_t nearest256(Rgb c);
// Nearest ANSI-16 index (FTXUI Palette16 order: 0-7 dark, 8-15 bright).
std::uint8_t nearest16(Rgb c);

// Mode-aware conversion into an ftxui::Color for drawing.
ftxui::Color to_ftxui(Rgb c, ColorMode mode);

struct Theme {
  // Every color in the UI comes from here; nothing is hard-coded elsewhere.
  Rgb accent;      // legend values, highlights
  Rgb title;       // page title (also used for panel titles)
  Rgb text;        // labels
  Rgb label_dim;   // secondary text, footer
  Rgb border;      // panel borders (and the y-axis separator)
  Rgb grid;        // dotted gridlines
  Rgb axis_text;   // y-axis tick labels
  Rgb warn;        // PAUSED, error-ish
  Rgb series[6];   // line colors, in legend order

  // Fill colors are derived per series as a tint (series × 0.45), so fills
  // read as a shade of the series color rather than grey. Phase 5 tunable.
  Rgb fill(int i) const { return scale(series[i], 0.45); }

  static Theme cyberpunk_blue();
};

}  // namespace geth::ui
