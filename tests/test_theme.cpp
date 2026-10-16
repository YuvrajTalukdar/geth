// Phase 5, Part B — themes: hex parsing, the exact built-in palettes, the
// injective 16-color series mapping, and custom theme files (base +
// inheritance, invalid values -> warning + base fallback, unknown keys,
// broken files, base cycles, --list-themes' all_themes()).

#include <doctest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <unistd.h>

#include "ui/theme.hpp"

using namespace geth::ui;

namespace {

// Point XDG_CONFIG_HOME at a private, empty-per-test themes directory and
// restore the environment on scope exit. themes_dir() reads the env on every
// call, so this is safe.
struct ThemeDirGuard {
  std::string old_xdg;
  bool had_xdg = false;
  std::string dir;

  ThemeDirGuard() {
    if (const char* e = std::getenv("XDG_CONFIG_HOME")) {
      old_xdg = e;
      had_xdg = true;
    }
    dir = (std::filesystem::temp_directory_path() /
           ("geth_theme_test_" + std::to_string(::getpid())))
              .string();
    std::error_code ec;
    std::filesystem::create_directories(dir + "/geth/themes", ec);
    setenv("XDG_CONFIG_HOME", dir.c_str(), 1);
  }
  ~ThemeDirGuard() {
    if (had_xdg)
      setenv("XDG_CONFIG_HOME", old_xdg.c_str(), 1);
    else
      unsetenv("XDG_CONFIG_HOME");
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }

  // Writes a theme file and returns its name.
  std::string write(const std::string& name, const std::string& body) {
    std::ofstream f(dir + "/geth/themes/" + name + ".toml");
    f << body;
    return name;
  }
};

// The spec table, field by field.
const Rgb rgb(int r, int g, int b) {
  return {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
          static_cast<std::uint8_t>(b)};
}
// A single-call comparison: doctest cannot decompose `a == f(...)` (it trips
// its "Expression Too Complex" guard), so palette checks go through this.
bool same(const Rgb& a, const Rgb& b) { return a == b; }
// All 14 colors of a theme in a fixed order (8 fields + 6 series).
std::array<Rgb, 14> flat(const Theme& t) {
  return {t.accent, t.title,    t.text,      t.label_dim, t.border,  t.grid,
          t.axis_text, t.warn,  t.series[0], t.series[1], t.series[2], t.series[3],
          t.series[4], t.series[5]};
}
bool same_flat(const std::array<Rgb, 14>& a, const std::array<Rgb, 14>& b) {
  return a == b;
}

}  // namespace

TEST_SUITE("theme") {

TEST_CASE("parse_hex: #RRGGBB, case-insensitive; rejects the rest") {
  Rgb c{};
  CHECK(parse_hex("#00E5FF", c));
  CHECK(same(c, rgb(0x00, 0xE5, 0xFF)));
  CHECK(parse_hex("#abcDEF", c));
  CHECK(same(c, rgb(0xAB, 0xCD, 0xEF)));
  CHECK_FALSE(parse_hex("00E5FF", c));    // no #
  CHECK_FALSE(parse_hex("#00E5", c));     // too short
  CHECK_FALSE(parse_hex("#00E5FFFF", c)); // too long
  CHECK_FALSE(parse_hex("#GGIIJJ", c));   // not hex
  CHECK_FALSE(parse_hex("", c));
}

TEST_CASE("built-in palettes match the spec table exactly") {
  // flat() order: accent, title, text, label_dim, border, grid, axis_text,
  // warn, series_1..series_6. (title equals accent in all four themes.)
  const std::array<Rgb, 14> cyber = {
      rgb(0x00, 0xE5, 0xFF), rgb(0x00, 0xE5, 0xFF), rgb(0x9E, 0xEF, 0xFF),
      rgb(0x4F, 0xA3, 0xB3), rgb(0x0F, 0x5A, 0x66), rgb(0x0A, 0x3A, 0x42),
      rgb(0x3E, 0x8C, 0x99), rgb(0xFF, 0x3B, 0x5C), rgb(0x00, 0xE5, 0xFF),
      rgb(0x39, 0xFF, 0x88), rgb(0xFF, 0x4F, 0xD8), rgb(0xFF, 0xD2, 0x3F),
      rgb(0x4D, 0xA3, 0xFF), rgb(0xC7, 0x7D, 0xFF)};
  const std::array<Rgb, 14> matrix = {
      rgb(0x39, 0xFF, 0x14), rgb(0x39, 0xFF, 0x14), rgb(0xB6, 0xFF, 0xB0),
      rgb(0x4F, 0xAF, 0x5F), rgb(0x0F, 0x5A, 0x2A), rgb(0x0A, 0x3A, 0x1C),
      rgb(0x3E, 0x8C, 0x55), rgb(0xFF, 0x3B, 0x5C), rgb(0x39, 0xFF, 0x14),
      rgb(0x00, 0xE5, 0xFF), rgb(0xFF, 0xD2, 0x3F), rgb(0xFF, 0x4F, 0xD8),
      rgb(0x7C, 0xFF, 0xCB), rgb(0xC7, 0x7D, 0xFF)};
  const std::array<Rgb, 14> crimson = {
      rgb(0xFF, 0x2E, 0x4D), rgb(0xFF, 0x2E, 0x4D), rgb(0xFF, 0xC2, 0xCB),
      rgb(0xB3, 0x50, 0x5F), rgb(0x5A, 0x14, 0x20), rgb(0x3A, 0x0D, 0x15),
      rgb(0x8C, 0x3E, 0x4A), rgb(0xFF, 0xD2, 0x3F), rgb(0xFF, 0x2E, 0x4D),
      rgb(0xFF, 0xB0, 0x00), rgb(0x00, 0xE5, 0xFF), rgb(0xFF, 0x7A, 0xE0),
      rgb(0xFF, 0x8A, 0x3D), rgb(0xC7, 0x7D, 0xFF)};
  const std::array<Rgb, 14> amber = {
      rgb(0xFF, 0xB0, 0x00), rgb(0xFF, 0xB0, 0x00), rgb(0xFF, 0xE2, 0xA8),
      rgb(0xB3, 0x89, 0x3E), rgb(0x5A, 0x41, 0x10), rgb(0x3A, 0x2A, 0x0A),
      rgb(0x8C, 0x6D, 0x3E), rgb(0xFF, 0x3B, 0x5C), rgb(0xFF, 0xB0, 0x00),
      rgb(0x00, 0xE5, 0xFF), rgb(0xFF, 0x4F, 0xD8), rgb(0x39, 0xFF, 0x88),
      rgb(0xFF, 0x6A, 0x3D), rgb(0xC7, 0x7D, 0xFF)};

  const Theme cb = Theme::cyberpunk_blue();
  const Theme mg = Theme::matrix_green();
  const Theme cr = Theme::crimson_red();
  const Theme am = Theme::amber();
  CHECK(same_flat(flat(cb), cyber));
  CHECK(same_flat(flat(mg), matrix));
  CHECK(same_flat(flat(cr), crimson));
  CHECK(same_flat(flat(am), amber));
  CHECK(cb.fill_mix == doctest::Approx(0.45));
  CHECK(mg.fill_mix == doctest::Approx(0.45));
  CHECK(cr.fill_mix == doctest::Approx(0.45));
  CHECK(am.fill_mix == doctest::Approx(0.45));
}

TEST_CASE("16-color mapping: six distinct slots for every built-in theme") {
  for (const auto& b : builtin_themes()) {
    std::uint8_t slots[6];
    series16_slots(b.theme.series, slots);
    bool distinct = true;
    for (int i = 0; i < 6 && distinct; ++i)
      for (int j = i + 1; j < 6; ++j)
        if (slots[i] == slots[j]) distinct = false;
    (void)b;
    CHECK(distinct);
  }
}

TEST_CASE("resolve_theme: built-in names, unknown name -> nullopt") {
  std::vector<std::string> warn;
  CHECK(resolve_theme("cyberpunk-blue", &warn).has_value());
  CHECK(resolve_theme("matrix-green", &warn).has_value());
  CHECK(resolve_theme("crimson-red", &warn).has_value());
  CHECK(resolve_theme("amber", &warn).has_value());
  CHECK_FALSE(resolve_theme("does-not-exist", &warn).has_value());
}

TEST_CASE("custom theme: base + inheritance, missing fields inherit") {
  ThemeDirGuard g;
  const std::string n = g.write("child",
      "base = \"cyberpunk-blue\"\n"
      "accent = \"#112233\"\n"
      "series_2 = \"#445566\"\n");
  std::vector<std::string> warn;
  const auto t = resolve_theme(n, &warn);
  REQUIRE(t.has_value());
  CHECK(warn.empty());
  // Overridden:
  CHECK(same((*t).accent, rgb(0x11, 0x22, 0x33)));
  CHECK(same((*t).series[1], rgb(0x44, 0x55, 0x66)));
  // Inherited from cyberpunk-blue:
  CHECK(same((*t).title, rgb(0x00, 0xE5, 0xFF)));
  CHECK(same((*t).series[0], rgb(0x00, 0xE5, 0xFF)));
  CHECK(same((*t).border, rgb(0x0F, 0x5A, 0x66)));
}

TEST_CASE("custom theme: invalid hex / out-of-range fill_mix -> warn + base value") {
  ThemeDirGuard g;
  const std::string n = g.write("badvals",
      "base = \"cyberpunk-blue\"\n"
      "accent = \"#GGHHII\"\n"       // not hex
      "warn = \"red\"\n"             // not #RRGGBB
      "fill_mix = 1.7\n"             // out of 0..1
      "series_1 = \"#12345\"\n");    // too short
  std::vector<std::string> warn;
  const auto t = resolve_theme(n, &warn);
  REQUIRE(t.has_value());
  CHECK(warn.size() >= 4);
  // Every bad field kept the base value:
  CHECK(same((*t).accent, rgb(0x00, 0xE5, 0xFF)));
  CHECK(same((*t).warn, rgb(0xFF, 0x3B, 0x5C)));
  CHECK(same((*t).series[0], rgb(0x00, 0xE5, 0xFF)));
  CHECK((*t).fill_mix == doctest::Approx(0.45));
}

TEST_CASE("custom theme: unknown key -> warning, value ignored") {
  ThemeDirGuard g;
  const std::string n = g.write("unknownkey",
      "base = \"amber\"\n"
      "accent = \"#001100\"\n"
      "nonsense_key = \"#FF0000\"\n");
  std::vector<std::string> warn;
  const auto t = resolve_theme(n, &warn);
  REQUIRE(t.has_value());
  CHECK(warn.size() == 1);
  CHECK(warn[0].find("nonsense_key") != std::string::npos);
  CHECK(same((*t).accent, rgb(0x00, 0x11, 0x00)));
}

TEST_CASE("custom theme: unknown base / missing file / broken file -> never throws") {
  ThemeDirGuard g;
  g.write("orphan", "base = \"no-such-base\"\naccent = \"#001100\"\n");
  std::vector<std::string> warn;
  // Unknown base: warning, and the theme still resolves with defaults+overrides.
  const auto t = resolve_theme("orphan", &warn);
  CHECK(t.has_value());
  CHECK(warn.size() == 1);

  // Missing file: nullopt.
  warn.clear();
  CHECK_FALSE(resolve_theme("no-such-theme", &warn).has_value());

  // Broken TOML: nullopt, no throw.
  g.write("broken", "this is [ not toml = = =");
  warn.clear();
  CHECK_FALSE(resolve_theme("broken", &warn).has_value());
}

TEST_CASE("custom theme: base cycles are detected, not infinite") {
  ThemeDirGuard g;
  g.write("a", "base = \"b\"\naccent = \"#010203\"\n");
  g.write("b", "base = \"a\"\n");
  std::vector<std::string> warn;
  const auto t = resolve_theme("a", &warn);
  (void)t;  // the point is that it terminates; the warning explains the loop
  CHECK(warn.size() >= 1);
}

TEST_CASE("custom theme: custom-on-custom layering") {
  ThemeDirGuard g;
  g.write("parent", "base = \"matrix-green\"\nborder = \"#001111\"\n");
  const std::string n = g.write("kid",
      "base = \"parent\"\n"
      "accent = \"#001122\"\n");
  std::vector<std::string> warn;
  const auto t = resolve_theme(n, &warn);
  REQUIRE(t.has_value());
  CHECK(warn.empty());
  CHECK(same((*t).accent, rgb(0x00, 0x11, 0x22)));
  CHECK(same((*t).border, rgb(0x00, 0x11, 0x11)));   // from the parent
  CHECK(same((*t).grid, rgb(0x0A, 0x3A, 0x1C)));     // from matrix-green
}

TEST_CASE("all_themes: built-ins, then custom files (sorted); broken files skipped") {
  ThemeDirGuard g;
  g.write("zeta", "accent = \"#010101\"\n");
  g.write("alpha", "accent = \"#020202\"\n");
  g.write("broken2", "not toml [ [");
  std::vector<std::string> warn;
  const auto all = all_themes(&warn);
  // 4 built-ins + alpha + zeta (broken2 does not resolve -> not selectable).
  REQUIRE_EQ(all.size(), 6u);
  CHECK_EQ(all[0].first, std::string("cyberpunk-blue"));
  CHECK_EQ(all[1].first, std::string("matrix-green"));
  CHECK_EQ(all[2].first, std::string("crimson-red"));
  CHECK_EQ(all[3].first, std::string("amber"));
  CHECK_EQ(all[4].first, std::string("alpha"));
  CHECK_EQ(all[5].first, std::string("zeta"));
}

TEST_CASE("theme_file_path: name validation") {
  CHECK(!theme_file_path("ok-name_1.x").empty());
  CHECK(theme_file_path("").empty());
  CHECK(theme_file_path("..").empty());
  CHECK(theme_file_path(".").empty());
  CHECK(theme_file_path("a/b").empty());   // path separator
  CHECK(theme_file_path("a b").empty());   // space
}

}  // TEST_SUITE
