// Color themes for the UI (Phase 5).
//
// A Theme is a plain struct of 24-bit RGB fields; every UI color is read from
// it, nothing is hard-coded elsewhere. Colors are converted to the terminal's
// color support at draw time:
//   truecolor  -> 38;2;R;G;B (when COLORTERM is truecolor/24bit)
//   256        -> nearest xterm-256 color
//   16         -> an injective ANSI-16 assignment (no two series share a slot)
// `--color=truecolor|256|16` overrides the auto-detection.
//
// Background: the UI is TRANSPARENT by default and never paints a background.
// The one exception is the config setting background = "solid", which paints
// Theme::bg across the whole screen — the only code path allowed to emit a
// background color (see render_frame's final pass).
//
// Built-in themes: cyberpunk-blue, matrix-green, crimson-red, amber,
// blaze-orange.
// Custom themes: $XDG_CONFIG_HOME/geth/themes/<name>.toml (or ~/.config),
// the same fields plus base = "<theme>" — a field left out keeps the base's
// value. Invalid hex values and unknown fields warn and fall back to the
// base value; a custom theme never crashes the app.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ftxui/screen/color.hpp"

namespace geth::ui {

enum class ColorMode { TrueColor, Color256, Color16 };

// Parses "truecolor"|"24bit"|"256"|"16"; false on anything else.
bool parse_color_mode(const std::string& s, ColorMode& out);

// Auto-detect: truecolor when COLORTERM is truecolor or 24bit, else 256.
ColorMode detect_color_mode();

struct Rgb {
  std::uint8_t r = 0, g = 0, b = 0;
  bool operator==(const Rgb&) const = default;
};

constexpr Rgb rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) { return {r, g, b}; }

// Parses "#RRGGBB" (case-insensitive); false on anything else.
bool parse_hex(const std::string& s, Rgb& out);

// Component-wise scale (rounding to the nearest integer).
Rgb scale(Rgb c, double f);

// The fill tint: the series color scaled to `mix` (0.30 = the default, a
// darker shade of the series color). In TrueColor this is exactly what is
// drawn; in 256/16 it is drawn through fill_color(), which keeps the shade in
// the series' hue family.
Rgb derive_fill(Rgb series, double mix);

// Hue of c in [0, 360) degrees, or -1 when achromatic (r == g == b).
double hue_deg(Rgb c);

// Nearest xterm-256 palette index for c.
std::uint8_t nearest256(Rgb c);
// Nearest ANSI-16 index (FTXUI Palette16 order: 0-7 dark, 8-15 bright).
std::uint8_t nearest16(Rgb c);

// The ANSI-16 slots for a theme's SIX series, assigned INJECTIVELY when
// possible: pairs (series, slot) are matched by ascending RGB distance and a
// series only takes a slot a nearer pair did not already claim. With six
// series and sixteen slots the result is always six distinct slots, so two
// series never share a color on a 16-color terminal.
void series16_slots(const Rgb (&series)[6], std::uint8_t out[6]);

// Mode-aware conversion into an ftxui::Color for drawing.
ftxui::Color to_ftxui(Rgb c, ColorMode mode);

// The drawable fill for a series color: derive_fill(series, mix) exactly in
// TrueColor; in 256/16 the nearest palette color of the same hue family as
// `series`. Even on the correct xterm cube, plain nearest-neighbor collapses
// the green fill (26,115,61) onto (0,95,95) — the border's own teal — and the
// blue fill onto the same teal, so the fill is quantized with the series'
// hue as a constraint (within 30 degrees) instead.
ftxui::Color fill_color(Rgb series, ColorMode mode, double mix);

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
  // Line colors by ROLE (Phase 6, Part C): slot 1 = CPU/primary, slot 2 =
  // first dGPU, slot 3 = iGPU/secondary, slot 4 = second dGPU, slots 5-6 =
  // slot 1 / slot 2 mixed 40 % toward white. Every theme's four role colors
  // are validated (OKLab ΔE + dichromat simulation + contrast vs the bg) —
  // see validate_series_palette and the palette tests.
  Rgb series[6];

  // Painted across the whole screen ONLY when background = "solid" (the
  // single sanctioned background path). Never used in transparent mode.
  Rgb bg;
  // The fill tint fraction (0.30 = the default), per theme.
  double fill_mix = 0.30;

  // Fill colors are derived per series as a tint, so fills read as a shade
  // of the series color rather than grey.
  Rgb fill(int i) const { return derive_fill(series[i], fill_mix); }

  static Theme cyberpunk_blue();
  static Theme matrix_green();
  static Theme crimson_red();
  static Theme amber();
  static Theme blaze_orange();
};

// The built-in themes, in `T` cycle order.
struct BuiltinTheme {
  const char* name;
  Theme theme;
};
const std::array<BuiltinTheme, 5>& builtin_themes();

// ---------------------------------------------------------------------------
// Phase 6 (Part C) palette validation.
//
// OKLab (Ottosson) color difference, scaled x100, between two sRGB colors.
double delta_e_oklab100(Rgb a, Rgb b);
// The same after a Machado et al. (2009) full-severity dichromat simulation
// of the pair: condition "protan" (protanopia) or "deutan" (deuteranopia);
// the matrices are applied to LINEAR sRGB, then the simulated pair is
// compared in OKLab (x100).
double delta_e_simulated100(Rgb a, Rgb b, const std::string& condition);
// WCAG 2.x contrast ratio (1..21) between two sRGB colors.
double contrast_ratio(Rgb a, Rgb b);

// Validates a theme's series palette. Rules (one string per violation,
// empty = valid):
//   - normal vision:   ΔE(OKLab x100) >= 15 for every pair among slots 1-4
//   - protan + deutan: ΔE >= 8 for every pair among slots 1-4, EXCEPT
//     matrix-green's slots 1<->4 (measured 7.1 deutan / 7.9 protan; allowed
//     at >= 7.0 for both because every series carries a labelled legend)
//   - contrast >= 3:1 against #0B0F14 for all six slots
// Custom themes are checked at load time (a violation is a WARNING — a toast
// in the app — and the theme still loads); the unit tests print the full
// measured matrix for the built-ins.
std::vector<std::string> validate_series_palette(const std::string& name,
                                                 const Theme& t);

// $XDG_CONFIG_HOME/geth/themes/<name>.toml, or
// ~/.config/geth/themes/<name>.toml. `name` is validated ([A-Za-z0-9._-],
// no path separators, not "." or "..") — an invalid name yields "".
std::string theme_file_path(const std::string& name);

// Resolves a theme by name: a built-in (exact name) or a custom file (the
// file's base + overrides). Custom file fields: base, accent, title, text,
// label_dim, border, grid, axis_text, warn, bg, fill_mix, series_1..series_6.
// Every problem — unparseable file, unknown base, invalid hex, out-of-range
// fill_mix, unknown key — appends a warning to `warnings` and falls back to
// the base value (or the default theme); it never throws.
// nullopt when the name resolves to nothing.
std::optional<Theme> resolve_theme(const std::string& name,
                                   std::vector<std::string>* warnings);

// Every selectable theme, in `T` cycle order: the built-ins, then the custom
// files in the themes directory (sorted by name). Problems resolving a custom
// file append warnings; the file is simply not selectable.
std::vector<std::pair<std::string, Theme>> all_themes(
    std::vector<std::string>* warnings);

}  // namespace geth::ui
