#include "ui/theme.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <string_view>

#include <toml++/toml.hpp>

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

bool parse_hex(const std::string& s, Rgb& out) {
  if (s.size() != 7 || s[0] != '#') return false;
  auto hexval = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  int v[3];
  for (int i = 0; i < 3; ++i) {
    const int hi = hexval(s[1 + i * 2]);
    const int lo = hexval(s[2 + i * 2]);
    if (hi < 0 || lo < 0) return false;
    v[i] = hi * 16 + lo;
  }
  out = {static_cast<std::uint8_t>(v[0]), static_cast<std::uint8_t>(v[1]),
         static_cast<std::uint8_t>(v[2])};
  return true;
}

Rgb scale(Rgb c, double f) {
  return {static_cast<std::uint8_t>(c.r * f + 0.5),
          static_cast<std::uint8_t>(c.g * f + 0.5),
          static_cast<std::uint8_t>(c.b * f + 0.5)};
}

Rgb derive_fill(Rgb series, double mix) { return scale(series, mix); }

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

// $XDG_CONFIG_HOME/geth/themes, or ~/.config/geth/themes (or ./... as a
// last resort, mirroring Config::default_path()).
std::string themes_dir() {
  const char* xdg = std::getenv("XDG_CONFIG_HOME");
  std::string base;
  if (xdg && *xdg)
    base = xdg;
  else if (const char* home = std::getenv("HOME"))
    base = std::string(home) + "/.config";
  else
    base = ".";
  return base + "/geth/themes";
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

void series16_slots(const Rgb (&series)[6], std::uint8_t out[6]) {
  struct Pair {
    long long d;
    int s;    // series index
    int slot; // ANSI-16 index
  };
  std::vector<Pair> all;
  all.reserve(6 * 16);
  for (int s = 0; s < 6; ++s)
    for (int i = 0; i < 16; ++i)
      all.push_back({sqdist(series[s], kAnsi16[i]), s, i});
  std::sort(all.begin(), all.end(), [](const Pair& a, const Pair& b) {
    if (a.d != b.d) return a.d < b.d;
    if (a.s != b.s) return a.s < b.s;
    return a.slot < b.slot;
  });
  // Greedy: the closest unclaimed (series, slot) pair wins; a series takes
  // its first free slot, a slot is claimed by its first series.
  bool s_done[6] = {false};
  bool slot_taken[16] = {false};
  for (const auto& p : all) {
    if (s_done[p.s] || slot_taken[p.slot]) continue;
    s_done[p.s] = true;
    slot_taken[p.slot] = true;
    out[p.s] = static_cast<std::uint8_t>(p.slot);
  }
  // With 6 series and 16 slots every series is matched; this is a safety
  // net for the impossible (fall back to plain nearest).
  for (int s = 0; s < 6; ++s)
    if (!s_done[s]) out[s] = nearest16(series[s]);
}

ftxui::Color to_ftxui(Rgb c, ColorMode mode) {
  switch (mode) {
    case ColorMode::TrueColor: return ftxui::Color::RGB(c.r, c.g, c.b);
    case ColorMode::Color256: return ftxui::Color(ftxui::Color::Palette256(nearest256(c)));
    case ColorMode::Color16: return ftxui::Color(ftxui::Color::Palette16(nearest16(c)));
  }
  return ftxui::Color::RGB(c.r, c.g, c.b);
}

ftxui::Color fill_color(Rgb series, ColorMode mode, double mix) {
  const Rgb target = derive_fill(series, mix);
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
  t.accent = rgb(0x00, 0xE5, 0xFF);    // #00E5FF
  t.title = rgb(0x00, 0xE5, 0xFF);     // #00E5FF
  t.text = rgb(0x9E, 0xEF, 0xFF);      // #9EEFFF
  t.label_dim = rgb(0x4F, 0xA3, 0xB3);  // #4FA3B3
  t.border = rgb(0x0F, 0x5A, 0x66);     // #0F5A66
  t.grid = rgb(0x0A, 0x3A, 0x42);       // #0A3A42
  t.axis_text = rgb(0x3E, 0x8C, 0x99);  // #3E8C99
  t.warn = rgb(0xFF, 0x3B, 0x5C);       // #FF3B5C
  t.bg = rgb(0x0B, 0x0F, 0x14);         // #0B0F14 (solid mode)
  t.series[0] = rgb(0x00, 0xE5, 0xFF);  // #00E5FF cyan
  t.series[1] = rgb(0x39, 0xFF, 0x88);  // #39FF88 green
  t.series[2] = rgb(0xFF, 0x4F, 0xD8);  // #FF4FD8 pink
  t.series[3] = rgb(0xFF, 0xD2, 0x3F);  // #FFD23F amber
  t.series[4] = rgb(0x4D, 0xA3, 0xFF);  // #4DA3FF blue
  t.series[5] = rgb(0xC7, 0x7D, 0xFF);  // #C77DFF violet
  return t;
}

Theme Theme::matrix_green() {
  Theme t;
  t.accent = rgb(0x39, 0xFF, 0x14);    // #39FF14
  t.title = rgb(0x39, 0xFF, 0x14);     // #39FF14
  t.text = rgb(0xB6, 0xFF, 0xB0);      // #B6FFB0
  t.label_dim = rgb(0x4F, 0xAF, 0x5F); // #4FAF5F
  t.border = rgb(0x0F, 0x5A, 0x2A);    // #0F5A2A
  t.grid = rgb(0x0A, 0x3A, 0x1C);      // #0A3A1C
  t.axis_text = rgb(0x3E, 0x8C, 0x55); // #3E8C55
  t.warn = rgb(0xFF, 0x3B, 0x5C);      // #FF3B5C
  t.bg = rgb(0x0A, 0x13, 0x0A);        // #0A130A (solid mode)
  t.series[0] = rgb(0x39, 0xFF, 0x14);  // #39FF14 green
  t.series[1] = rgb(0x00, 0xE5, 0xFF);  // #00E5FF cyan
  t.series[2] = rgb(0xFF, 0xD2, 0x3F);  // #FFD23F amber
  t.series[3] = rgb(0xFF, 0x4F, 0xD8);  // #FF4FD8 pink
  t.series[4] = rgb(0x7C, 0xFF, 0xCB);  // #7CFFCB aqua
  t.series[5] = rgb(0xC7, 0x7D, 0xFF);  // #C77DFF violet
  return t;
}

Theme Theme::crimson_red() {
  Theme t;
  t.accent = rgb(0xFF, 0x2E, 0x4D);    // #FF2E4D
  t.title = rgb(0xFF, 0x2E, 0x4D);     // #FF2E4D
  t.text = rgb(0xFF, 0xC2, 0xCB);      // #FFC2CB
  t.label_dim = rgb(0xB3, 0x50, 0x5F); // #B3505F
  t.border = rgb(0x5A, 0x14, 0x20);    // #5A1420
  t.grid = rgb(0x3A, 0x0D, 0x15);      // #3A0D15
  t.axis_text = rgb(0x8C, 0x3E, 0x4A); // #8C3E4A
  t.warn = rgb(0xFF, 0xD2, 0x3F);      // #FFD23F
  t.bg = rgb(0x14, 0x0A, 0x0E);        // #140A0E (solid mode)
  t.series[0] = rgb(0xFF, 0x2E, 0x4D); // #FF2E4D red
  t.series[1] = rgb(0xFF, 0xB0, 0x00); // #FFB000 amber
  t.series[2] = rgb(0x00, 0xE5, 0xFF); // #00E5FF cyan
  t.series[3] = rgb(0xFF, 0x7A, 0xE0); // #FF7AE0 pink
  t.series[4] = rgb(0xFF, 0x8A, 0x3D); // #FF8A3D orange
  t.series[5] = rgb(0xC7, 0x7D, 0xFF); // #C77DFF violet
  return t;
}

Theme Theme::amber() {
  Theme t;
  t.accent = rgb(0xFF, 0xB0, 0x00);    // #FFB000
  t.title = rgb(0xFF, 0xB0, 0x00);     // #FFB000
  t.text = rgb(0xFF, 0xE2, 0xA8);      // #FFE2A8
  t.label_dim = rgb(0xB3, 0x89, 0x3E); // #B3893E
  t.border = rgb(0x5A, 0x41, 0x10);    // #5A4110
  t.grid = rgb(0x3A, 0x2A, 0x0A);      // #3A2A0A
  t.axis_text = rgb(0x8C, 0x6D, 0x3E); // #8C6D3E
  t.warn = rgb(0xFF, 0x3B, 0x5C);      // #FF3B5C
  t.bg = rgb(0x14, 0x10, 0x08);        // #141008 (solid mode)
  t.series[0] = rgb(0xFF, 0xB0, 0x00); // #FFB000 amber
  t.series[1] = rgb(0x00, 0xE5, 0xFF); // #00E5FF cyan
  t.series[2] = rgb(0xFF, 0x4F, 0xD8); // #FF4FD8 pink
  t.series[3] = rgb(0x39, 0xFF, 0x88); // #39FF88 green
  t.series[4] = rgb(0xFF, 0x6A, 0x3D); // #FF6A3D orange
  t.series[5] = rgb(0xC7, 0x7D, 0xFF); // #C77DFF violet
  return t;
}

const std::array<BuiltinTheme, 4>& builtin_themes() {
  // Built element-by-element (not a braced-init-list): the nested braces of a
  // std::array of aggregates trip a GCC brace-matching false positive.
  static const std::array<BuiltinTheme, 4> kThemes = [] {
    std::array<BuiltinTheme, 4> a{};
    a[0] = BuiltinTheme{"cyberpunk-blue", Theme::cyberpunk_blue()};
    a[1] = BuiltinTheme{"matrix-green", Theme::matrix_green()};
    a[2] = BuiltinTheme{"crimson-red", Theme::crimson_red()};
    a[3] = BuiltinTheme{"amber", Theme::amber()};
    return a;
  }();
  return kThemes;
}

std::string theme_file_path(const std::string& name) {
  if (name.empty() || name == "." || name == "..") return "";
  for (char c : name)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' &&
        c != '_' && c != '-')
      return "";  // path separator, space, ...: not a theme name
  return themes_dir() + "/" + name + ".toml";
}

namespace {

// The built-in theme `name`, or nullopt.
std::optional<Theme> builtin_by_name(const std::string& name) {
  for (const auto& b : builtin_themes())
    if (name == b.name) return b.theme;
  return std::nullopt;
}

// The custom theme file `name`, on top of its resolved base. `visited`
// breaks base chains that loop.
std::optional<Theme> load_custom(const std::string& name,
                                 std::vector<std::string>& warnings,
                                 std::set<std::string>& visited) {
  // Unresolvable file -> nullopt (the caller falls back: a top-level resolve
  // reports the unknown name, a nested base keeps its default base).
  const std::string path = theme_file_path(name);
  if (path.empty()) {
    warnings.push_back("theme '" + name + "': not a valid theme name");
    return std::nullopt;
  }
  std::error_code ec;
  if (!std::filesystem::is_regular_file(path, ec)) {
    warnings.push_back("theme '" + name + "': " + path + " does not exist");
    return std::nullopt;
  }
  toml::table tbl;
  try {
    tbl = toml::parse_file(path);
  } catch (const toml::parse_error& e) {
    warnings.push_back("theme '" + name + "': " + path + " does not parse (" +
                       std::string(e.description()) + ")");
    return std::nullopt;
  }

  // base = "<theme>" — a built-in or another custom theme.
  std::string base_name = "cyberpunk-blue";
  Theme base = Theme::cyberpunk_blue();
  if (tbl.contains("base")) {
    const toml::node& bv = tbl.at("base");
    if (bv.is_string()) {
      base_name = bv.as<std::string>()->get();
      if (auto b = builtin_by_name(base_name)) {
        base = *b;
      } else if (visited.insert(base_name).second) {
        if (auto b = load_custom(base_name, warnings, visited)) base = *b;
      } else {
        warnings.push_back("theme '" + name + "': base '" + base_name +
                           "' is a cycle — using cyberpunk-blue");
      }
    } else {
      warnings.push_back("theme '" + name + "': base must be a string — "
                         "using cyberpunk-blue");
    }
  }

  Theme t = base;
  // One helper per field kind; a bad value warns and keeps the base value.
  auto color_field = [&](const char* key, Rgb& field) {
    if (!tbl.contains(key)) return;
    const toml::node& v = tbl.at(key);
    if (v.is_string()) {
      Rgb c;
      if (parse_hex(v.as<std::string>()->get(), c))
        field = c;
      else
        warnings.push_back("theme '" + name + "': " + std::string(key) +
                           " '" + v.as<std::string>()->get() +
                           "' is not #RRGGBB — keeping the base value");
    } else {
      warnings.push_back("theme '" + name + "': " + std::string(key) +
                         " must be a string — ignoring");
    }
  };
  static const char* const kSeriesKeys[6] = {
      "series_1", "series_2", "series_3", "series_4", "series_5", "series_6"};
  for (int i = 0; i < 6; ++i) color_field(kSeriesKeys[i], t.series[i]);
  color_field("accent", t.accent);
  color_field("title", t.title);
  color_field("text", t.text);
  color_field("label_dim", t.label_dim);
  color_field("border", t.border);
  color_field("grid", t.grid);
  color_field("axis_text", t.axis_text);
  color_field("warn", t.warn);
  color_field("bg", t.bg);

  if (tbl.contains("fill_mix")) {
    const toml::node& v = tbl.at("fill_mix");
    double f = -1.0;
    if (v.is_integer()) f = static_cast<double>(v.as<int64_t>()->get());
    else if (v.is_floating_point()) f = v.as<double>()->get();
    if (f >= 0.0 && f <= 1.0) {
      t.fill_mix = f;
    } else if (v.is_integer() || v.is_floating_point()) {
      warnings.push_back("theme '" + name + "': fill_mix " +
                         std::to_string(f) +
                         " is outside 0..1 — keeping the base value");
    } else {
      warnings.push_back("theme '" + name + "': fill_mix must be a number — "
                         "ignoring");
    }
  }

  static const char* const kKnown[] = {"base",     "accent", "title",
                                       "text",     "label_dim", "border",
                                       "grid",     "axis_text", "warn",
                                       "bg",       "fill_mix",  "series_1",
                                       "series_2", "series_3",  "series_4",
                                       "series_5", "series_6"};
  for (auto& [k, v] : tbl) {
    (void)v;
    // Braces, not parens: `std::string kn(std::string_view(k))` would be a
    // function declaration (most vexing parse).
    const std::string kn{std::string_view(k)};
    bool is_known = false;
    for (const char* known_key : kKnown)
      if (kn == known_key) {
        is_known = true;
        break;
      }
    if (!is_known)
      warnings.push_back("theme '" + name + "': unknown key '" + kn +
                         "' — ignoring");
  }
  return t;
}

}  // namespace

std::optional<Theme> resolve_theme(const std::string& name,
                                   std::vector<std::string>* warnings) {
  if (auto b = builtin_by_name(name)) return b;
  std::vector<std::string> local;
  std::set<std::string> visited;
  visited.insert(name);
  return load_custom(name, warnings ? *warnings : local, visited);
}

std::vector<std::pair<std::string, Theme>> all_themes(
    std::vector<std::string>* warnings) {
  std::vector<std::pair<std::string, Theme>> out;
  for (const auto& b : builtin_themes()) out.push_back({b.name, b.theme});
  std::error_code ec;
  const std::string dir = themes_dir();
  if (std::filesystem::is_directory(dir, ec)) {
    std::vector<std::string> stems;
    for (auto& e : std::filesystem::directory_iterator(dir, ec)) {
      if (!e.is_regular_file()) continue;
      const std::string p = e.path().filename().string();
      if (p.size() < 5 || p.substr(p.size() - 5) != ".toml") continue;
      stems.push_back(p.substr(0, p.size() - 5));
    }
    std::sort(stems.begin(), stems.end());
    for (const auto& s : stems) {
      if (builtin_by_name(s)) continue;  // a custom file shadowing a built-in
      std::set<std::string> visited;
      visited.insert(s);
      std::vector<std::string> local;
      if (auto t = load_custom(s, local, visited)) {
        out.push_back({s, *t});
        if (warnings) warnings->insert(warnings->end(), local.begin(), local.end());
      }
    }
  }
  return out;
}

}  // namespace geth::ui
