// UI tests: the transparency guarantee (GETH never paints a background color)
// and the "never crash at any terminal size" requirement. These drive the real
// render_frame() through an offscreen ftxui::Screen and inspect the SGR output.

#include <doctest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "core/collector.hpp"
#include "sources/demo.hpp"
#include "ui/frame.hpp"
#include "ui/panel_spec.hpp"
#include "ui/theme.hpp"
#include "util/alloc_count.hpp"
#include "util/format.hpp"

#include "ui_test_util.hpp"

using namespace geth;

namespace {

// The Phase 3 default graph window (1 m). The tests predate the t-cycle; a
// fixed 60 s window keeps their data geometry identical to before.
constexpr int64_t kWinNs = 60LL * 1000000000LL;

// A LayoutCache pointed at `page` (what the app does). render_frame takes the
// cache by non-const reference, so each call site builds its own.
ui::LayoutCache cache_for(const std::vector<ui::PanelSpec>& page) {
  ui::LayoutCache c;
  c.page = &page;
  return c;
}

// Builds a demo collector with a populated 60 s window, plus the matching
// Hardware + page the UI would render.
struct UiFixture {
  Collector c;
  ui::Hardware hw;
  std::vector<ui::PanelSpec> page;

  UiFixture() {
    c.add(std::make_unique<DemoSource>());
    c.probe_all(/*warm_up=*/true);
    for (int i = 0; i < 70; ++i) c.sample();  // enough points for a full line
    hw = ui::hardware_from(c.latest(), "AMD Ryzen 5 5600H with Radeon Graphics");
    page = ui::build_default_page(hw);
  }
};

// has_forbidden_style now lives in ui_test_util.hpp (shared with
// test_dashboard.cpp).

void render_at(int W, int H, const UiFixture& fx, const ui::Theme& theme,
               ui::ColorMode mode) {
  ui::FrameState state;
  ui::LayoutCache cache = cache_for(fx.page);
  ftxui::Screen screen(W, H);
  screen.Clear();
  ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr, state,
                   theme, mode, /*paused=*/false, 1000, cache, 0, kWinNs);
  (void)screen.ToString();  // must not throw / crash
}

// ---------------------------------------------------------------------------
// History tests: a source that ramps CPU usage 0 -> 100 % across the window
// (one sample per second, stamped by the collector's virtual clock), with an
// optional NaN gap in the middle.

struct RampSource : Source {
  bool gap = false;
  const char* name() const override { return "ramp (test)"; }
  SourceStatus probe() override {
    SourceStatus st;
    st.available = true;
    st.description = name();
    return st;
  }
  void sample(Snapshot& out) override {
    if (!started) {
      t0 = out.time;
      started = true;
    }
    const double el = std::chrono::duration<double>(out.time - t0).count();
    if (gap && el >= 25.0 && el <= 35.0)
      out.cpu.usage_pct = std::nullopt;  // gap: the graph must leave it empty
    else
      out.cpu.usage_pct = std::clamp(el / 59.0 * 100.0, 0.0, 100.0);
  }
  TimePoint t0;
  bool started = false;
};

// Renders a single CPU panel on a 138x12 screen. The Phase 3 layout puts a
// lone panel in a 1-column grid at the full width: the panel fills (0,1) at
// 138x10, its graph area is 129 braille columns x 7 rows (258 x 28 dots),
// with gx = 8, gy = 2, and "100 %" as the widest tick label (L = 5:
// Wg = (138-2) - 5 - 2 = 129).
void render_ramp(bool gap, ftxui::Screen& screen, int color_index = 0) {
  Collector c;
  auto rs = std::make_unique<RampSource>();
  rs->gap = gap;
  c.add(std::move(rs));
  c.probe_all(/*warm_up=*/true);
  // Virtual clock, like the --render-frame demo warm-up: 60 samples, one
  // per second. The probe's throw-away sample stamps t0 at el = 0.
  TimePoint vnow = c.latest().time;
  for (int i = 0; i < 60; ++i) {
    vnow += std::chrono::seconds(1);
    c.set_sample_time(vnow);
    c.sample();
  }
  ui::Hardware hw;
  hw.cpu_name = "Test CPU";
  ui::PanelSpec p;
  p.id = "cpu";
  p.title = "CPU";
  ui::SeriesSpec sr;
  sr.id = "cpu.usage";
  sr.label = "CPU";
  sr.color_index = color_index;
  sr.fill = true;
  p.series.push_back(sr);
  std::vector<ui::PanelSpec> page{p};
  ui::FrameState state;
  ui::LayoutCache cache = cache_for(page);
  ui::render_frame(screen, hw, page, c.store(), c.latest(), nullptr, state,
                   ui::Theme::cyberpunk_blue(), ui::ColorMode::TrueColor,
                   /*paused=*/false, 1000, cache, 0, kWinNs);
}

// A source that holds CPU usage at 0 % and spikes to 100 % for ONE sample in
// the second-to-last second of the 60 s window: ...0, 0, 0(t-2s), 100(t-1s),
// 0(now). The two slopes span only ~5 dot columns each: 27 dot rows in ~5
// columns, i.e. genuinely steep (several dot rows per column).
struct SpikeSource : Source {
  const char* name() const override { return "spike (test)"; }
  SourceStatus probe() override {
    SourceStatus st;
    st.available = true;
    st.description = name();
    return st;
  }
  void sample(Snapshot& out) override {
    if (!started) {
      started = true;  // probe: throw-away, pins nothing (n stays 0)
      out.cpu.usage_pct = 0.0;
      return;
    }
    // Recorded samples run n = 0..59 (the probe does not increment n), so
    // n == 58 is the 59th of the 60 samples: 100 % one second before the end.
    out.cpu.usage_pct = (n == 58) ? 100.0 : 0.0;
    ++n;
  }
  int n = 0;
  bool started = false;
};

// Renders the spike above (fill OFF: the test is about the LINE dots).
void render_spike(ftxui::Screen& screen) {
  Collector c;
  c.add(std::make_unique<SpikeSource>());
  c.probe_all(/*warm_up=*/true);
  // The probe stamped t0. Sixty one-second samples: t0+1s .. t0+60s. The
  // window spans the whole history (load_live clamps the span to the
  // available history, so the window must be FILLED — a few samples alone
  // would be stretched across the graph). The spike samples land at t0+58s
  // (0), t0+59s (100), t0+60s (0): dot columns ~247, ~252, 257.
  TimePoint vnow = c.latest().time;
  for (int i = 1; i <= 60; ++i) {
    vnow += std::chrono::seconds(1);
    c.set_sample_time(vnow);
    c.sample();
  }
  ui::Hardware hw;
  hw.cpu_name = "Test CPU";
  ui::PanelSpec p;
  p.id = "cpu";
  p.title = "CPU";
  ui::SeriesSpec sr;
  sr.id = "cpu.usage";
  sr.label = "CPU";
  sr.color_index = 0;
  sr.fill = false;  // line dots only
  p.series.push_back(sr);
  std::vector<ui::PanelSpec> page{p};
  ui::FrameState state;
  ui::LayoutCache cache = cache_for(page);
  ui::render_frame(screen, hw, page, c.store(), c.latest(), nullptr, state,
                   ui::Theme::cyberpunk_blue(), ui::ColorMode::TrueColor,
                   /*paused=*/false, 1000, cache, 0, kWinNs);
}

// The braille dot mask of the cell at dot-column cc, dot-row cy/4 (graph
// origin gx, gy); 0 when the pixel holds no braille glyph. Braille is
// exactly U+2800..U+28FF (UTF-8 E2 A0 80 .. E2 A3 BF) — the box-drawing
// glyphs (grid "┈", axis "│") are E2 94/95 xx and must NOT be decoded as
// dots.
std::uint8_t mask_at(const ftxui::Screen& s, int gx, int gy, int cc, int cy) {
  const std::string& ch = s.at(gx + cc / 2, gy + cy);
  const unsigned char b0 = ch.size() >= 3 ? static_cast<unsigned char>(ch[0]) : 0;
  const unsigned char b1 = ch.size() >= 3 ? static_cast<unsigned char>(ch[1]) : 0;
  if (ch.size() != 3 || b0 != 0xE2 || b1 < 0xA0 || b1 > 0xA3) return 0;
  const unsigned hi = b1 & 0x1F;
  const unsigned lo = static_cast<unsigned char>(ch[2]) & 0x3F;
  return static_cast<std::uint8_t>((hi << 6) | lo);
}

// Topmost set dot row (0..4*Hg-1, 0 = top) in dot column cc, or -1 if empty.
int top_dot(const ftxui::Screen& s, int gx, int gy, int cc, int Hg) {
  for (int cy = 0; cy < Hg; ++cy)
    for (int r = 0; r < 4; ++r)
      if (mask_at(s, gx, gy, cc, cy) & (1u << ui::braille_bit(cc % 2, r)))
        return cy * 4 + r;
  return -1;
}

// Bottommost set dot row in dot column cc, or -1 if empty.
int bottom_dot(const ftxui::Screen& s, int gx, int gy, int cc, int Hg) {
  for (int cy = Hg - 1; cy >= 0; --cy)
    for (int r = 3; r >= 0; --r)
      if (mask_at(s, gx, gy, cc, cy) & (1u << ui::braille_bit(cc % 2, r)))
        return cy * 4 + r;
  return -1;
}

// Is every dot row between top and bottom (inclusive) set in dot column cc?
bool column_contiguous(const ftxui::Screen& s, int gx, int gy, int cc, int Hg) {
  const int t = top_dot(s, gx, gy, cc, Hg);
  const int b = bottom_dot(s, gx, gy, cc, Hg);
  if (t < 0) return true;
  for (int y = t; y <= b; ++y) {
    // y is a DOT row (0..4*Hg-1); mask_at wants the character row (y/4) and
    // the braille row within it (y%4).
    if (!(mask_at(s, gx, gy, cc, y / 4) & (1u << ui::braille_bit(cc % 2, y % 4))))
      return false;
  }
  return true;
}

// The palette-256 index of `c`, by rendering it into a 1x1 screen and
// parsing the SGR ("38;5;N"); -1 when the color is not palette-256.
int fg256(const ftxui::Color& c) {
  ftxui::Screen one(1, 1);
  one.Clear();
  ftxui::Pixel& p = one.PixelAt(0, 0);
  p.character = "x";
  p.foreground_color = c;
  const std::string t = one.ToString();
  const std::string pat = "\x1b[38;5;";
  const std::size_t pos0 = t.find(pat);
  if (pos0 == std::string::npos) return -1;
  int n = 0;
  std::size_t pos = pos0 + pat.size();
  while (pos < t.size() && std::isdigit(static_cast<unsigned char>(t[pos])))
    n = n * 10 + (t[pos++] - '0');
  return n;
}

// Test-side xterm-256 decode (mirrors theme.cpp's build_palette256).
ui::Rgb pal256_rgb(int i) {
  static const int levels[6] = {0, 95, 135, 175, 215, 255};
  static const ui::Rgb a16[16] = {
      {0, 0, 0},     {205, 0, 0},     {0, 205, 0},     {205, 205, 0},
      {0, 0, 238},   {205, 0, 205},   {0, 205, 205},   {229, 229, 229},
      {127, 127, 127}, {255, 0, 0},   {0, 255, 0},     {255, 255, 0},
      {0, 0, 255},   {255, 0, 255},   {0, 255, 255},   {255, 255, 255}};
  if (i < 16) return a16[i];
  if (i < 232) {
    const int c = i - 16;
    return {static_cast<std::uint8_t>(levels[c / 36]),
            static_cast<std::uint8_t>(levels[(c / 6) % 6]),
            static_cast<std::uint8_t>(levels[c % 6])};
  }
  const int g = 8 + (i - 232) * 10;
  return {static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(g),
          static_cast<std::uint8_t>(g)};
}

}  // namespace

TEST_SUITE("ui") {

TEST_CASE("no background color is ever emitted (160x48, all color modes)") {
  const UiFixture fx;
  const ui::Theme theme = ui::Theme::cyberpunk_blue();
  const ui::ColorMode modes[] = {ui::ColorMode::TrueColor, ui::ColorMode::Color256,
                                 ui::ColorMode::Color16};
  for (auto mode : modes) {
    ui::FrameState state;
    ui::LayoutCache cache = cache_for(fx.page);
    ftxui::Screen screen(160, 48);
    screen.Clear();
    ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr, state,
                     theme, mode, /*paused=*/false, 1000, cache, 0, kWinNs);
    const std::string out = screen.ToString();
    // Sanity: a frame was actually drawn (borders, title, footer).
    CHECK(!out.empty());
    CHECK(out.find('\xe2') != std::string::npos);  // some UTF-8 (box/braille) glyph
    CHECK(!has_forbidden_style(out));
  }
}

// v1.0.1 Part A.3: every BUILT-IN theme renders in all three color modes
// (paired with the 16-slot distinctness test in test_theme.cpp).
TEST_CASE("every built-in theme renders in truecolor, 256 and 16 colors") {
  const UiFixture fx;
  const ui::Theme themes[] = {ui::Theme::cyberpunk_blue(), ui::Theme::matrix_green(),
                              ui::Theme::crimson_red(), ui::Theme::amber()};
  const ui::ColorMode modes[] = {ui::ColorMode::TrueColor, ui::ColorMode::Color256,
                                 ui::ColorMode::Color16};
  for (const auto& theme : themes)
    for (auto mode : modes) {
      ui::FrameState state;
      ui::LayoutCache cache = cache_for(fx.page);
      ftxui::Screen screen(160, 48);
      screen.Clear();
      ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr, state,
                       theme, mode, /*paused=*/false, 1000, cache, 0, kWinNs);
      const std::string out = screen.ToString();
      CHECK(!out.empty());
      CHECK(out.find('\xe2') != std::string::npos);  // box/braille glyphs drawn
      CHECK(!has_forbidden_style(out));  // no background SGR in any mode
      if (mode == ui::ColorMode::TrueColor)
        CHECK(out.find("38;2;") != std::string::npos);  // 24-bit fg present
      else if (mode == ui::ColorMode::Color256)
        CHECK(out.find("38;5;") != std::string::npos);  // 256-color fg present
      else
        CHECK(out.find("38;5;") == std::string::npos);  // 16: palette SGRs only
    }
}

TEST_CASE("never crashes at tiny / degenerate terminal sizes") {
  const UiFixture fx;
  const ui::Theme theme = ui::Theme::cyberpunk_blue();
  const int sizes[][2] = {{1, 1}, {5, 3}, {39, 11}, {1, 40}, {40, 12}, {12, 40}};
  for (const auto& sz : sizes)
    render_at(sz[0], sz[1], fx, theme, ui::ColorMode::TrueColor);
  // And again in the 16-color mode, where the palette-16 background SGRs
  // (4X / 10X) would appear if the rule were violated.
  for (const auto& sz : sizes)
    render_at(sz[0], sz[1], fx, theme, ui::ColorMode::Color16);
}

TEST_CASE("below 40x12 shows the too-small notice, at 40x12 shows the frame") {
  const UiFixture fx;
  const ui::Theme theme = ui::Theme::cyberpunk_blue();
  {
    ui::FrameState state;
    ui::LayoutCache cache = cache_for(fx.page);
    ftxui::Screen screen(39, 11);
    screen.Clear();
    ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr, state,
                     theme, ui::ColorMode::TrueColor, false, 1000, cache, 0, kWinNs);
    const std::string out = ui::screen_to_text(screen);
    CHECK(out.find("terminal too small") != std::string::npos);
  }
  {
    ui::FrameState state;
    ui::LayoutCache cache = cache_for(fx.page);
    ftxui::Screen screen(40, 12);
    screen.Clear();
    ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr, state,
                     theme, ui::ColorMode::TrueColor, false, 1000, cache, 0, kWinNs);
    const std::string out = ui::screen_to_text(screen);
    CHECK(out.find("terminal too small") == std::string::npos);
  }
}

TEST_CASE("geometry self-check: exactly ROWS lines, each COLS display cols wide") {
  const UiFixture fx;
  const ui::Theme theme = ui::Theme::cyberpunk_blue();
  const int sizes[][2] = {{160, 48}, {120, 40}, {100, 30}, {80, 24}, {200, 60}};
  for (const auto& sz : sizes) {
    ui::FrameState state;
    ui::LayoutCache cache = cache_for(fx.page);
    ftxui::Screen screen(sz[0], sz[1]);
    screen.Clear();
    ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr,
                     state, theme, ui::ColorMode::TrueColor, false, 1000, cache, 0,
                     kWinNs);
    const ui::GeometryReport rep =
        ui::check_geometry(ui::screen_to_text(screen), sz[0]);
    CHECK_EQ(rep.lines, sz[1]);
    CHECK(rep.bad_lines.empty());
  }
}

TEST_CASE("geometry self-check reports offending line numbers on mismatch") {
  const ui::GeometryReport ok_rep = ui::check_geometry("abc\ndef", 3);
  CHECK(ok_rep.ok(2));
  // The second line is one column short -> reported as line 2.
  const ui::GeometryReport bad = ui::check_geometry("abc\nd", 3);
  CHECK(bad.lines == 2);
  CHECK(!bad.ok(2));
  CHECK_EQ(bad.bad_lines.size(), static_cast<std::size_t>(1));
  if (!bad.bad_lines.empty()) CHECK_EQ(bad.bad_lines[0], 2);
  // A fullwidth glyph occupies two display columns (wcwidth), not one
  // code point: "a<CJK>b" is 4 columns wide (split literal: the hex escape
  // would otherwise swallow the following 'b').
  CHECK(ui::check_geometry("a\xe4\xb8\xad" "b", 4).ok(1));
}

TEST_CASE("braille: all eight dots is U+28FF, each single dot is distinct") {
  // Spec table: (0,0)=0x01 (0,1)=0x02 (0,2)=0x04 (0,3)=0x40
  //             (1,0)=0x08 (1,1)=0x10 (1,2)=0x20 (1,3)=0x80
  const std::uint8_t expect[8] = {0x01, 0x02, 0x04, 0x40, 0x08, 0x10, 0x20, 0x80};
  std::uint8_t all = 0;
  for (int i = 0; i < 8; ++i) {
    CHECK_EQ(static_cast<std::uint8_t>(1u << ui::braille_bit(i / 4, i % 4)),
             expect[i]);
    all |= expect[i];
  }
  CHECK_EQ(all, static_cast<std::uint8_t>(0xFF));
  CHECK(ui::braille_char(0xFF) == "\xE2\xA3\xBF");  // U+28FF full cell
  for (int i = 0; i < 8; ++i)
    for (int j = i + 1; j < 8; ++j)
      CHECK(ui::braille_char(expect[i]) != ui::braille_char(expect[j]));
}

TEST_CASE("graph history: 0->100% ramp over 60 s at 60 columns is monotonic") {
  ftxui::Screen screen(138, 12);
  render_ramp(/*gap=*/false, screen);
  const int gx = 8, gy = 2, Wg = 129, Hg = 7;
  const int dot_cols = 2 * Wg, dot_rows = 4 * Hg;
  CHECK_EQ(top_dot(screen, gx, gy, 0, Hg), dot_rows - 1);  // 0 % -> bottom
  CHECK_EQ(top_dot(screen, gx, gy, dot_cols - 1, Hg), 0);  // 100 % -> top
  // Heights never decrease left -> right.
  for (int xc = 1; xc < dot_cols; ++xc)
    CHECK(top_dot(screen, gx, gy, xc, Hg) <= top_dot(screen, gx, gy, xc - 1, Hg));
}

TEST_CASE("graph history: a NaN gap renders as empty columns, never 0") {
  ftxui::Screen screen(138, 12);
  render_ramp(/*gap=*/true, screen);
  const int gx = 8, gy = 2, Hg = 7;
  // Gap el 25..35 s of the 59 s span -> dot columns ~109..153 (of 258).
  for (int xc = 115; xc <= 148; ++xc) CHECK_EQ(top_dot(screen, gx, gy, xc, Hg), -1);
  CHECK(top_dot(screen, gx, gy, 90, Hg) >= 0);  // data before the gap
  CHECK(top_dot(screen, gx, gy, 175, Hg) >= 0);  // data after the gap
}

TEST_CASE("filled graph: line in series color, fill below in the derived color") {
  ftxui::Screen screen(138, 12);
  render_ramp(/*gap=*/false, screen);
  const int gx = 8, gy = 2, Wg = 129, Hg = 7;
  const int dot_cols = 2 * Wg;
  const ui::Theme theme = ui::Theme::cyberpunk_blue();
  const ftxui::Color line_c = ui::to_ftxui(theme.series[0], ui::ColorMode::TrueColor);
  const ftxui::Color fill_c = ui::to_ftxui(theme.fill(0), ui::ColorMode::TrueColor);
  CHECK(line_c != fill_c);  // the theme actually derives a distinct fill

  // Each dot column's line row = its topmost dot (a filled series fills from
  // the line down to the bottom, so the topmost dot IS the line).
  std::vector<int> line_row(dot_cols, -1);
  for (int cc = 0; cc < dot_cols; ++cc)
    line_row[cc] = top_dot(screen, gx, gy, cc, Hg);
  for (int cc = 0; cc < dot_cols; ++cc) REQUIRE(line_row[cc] >= 0);

  // A braille cell holds TWO dot columns, so its one color is the topmost
  // dot's color across both: the line color iff either sub-column's line
  // falls inside it, otherwise the derived fill color (the line, prio 1,
  // beats fill, prio 0, within a cell). Empty cells (grid only) are skipped.
  for (int ccol = 0; ccol < Wg; ++ccol)
    for (int cy = 0; cy < Hg; ++cy) {
      if (mask_at(screen, gx, gy, 2 * ccol, cy) == 0) continue;  // grid/empty
      const int lo = 4 * cy, hi = 4 * cy + 3;
      const bool has_line = (line_row[2 * ccol] >= lo && line_row[2 * ccol] <= hi) ||
                            (line_row[2 * ccol + 1] >= lo && line_row[2 * ccol + 1] <= hi);
      CHECK_EQ(screen.PixelAt(gx + ccol, gy + cy).foreground_color,
               has_line ? line_c : fill_c);
    }
}

TEST_CASE("bytes/sec auto-scale: 15 MiB/s -> 16 MiB/s, not 1 GiB/s") {
  const double KiB = 1024.0, MiB = 1024.0 * KiB;
  CHECK_EQ(ui::nice_bytes(15.0 * MiB), 16.0 * MiB);
  CHECK_EQ(ui::nice_bytes(100.0 * MiB), 128.0 * MiB);
  CHECK_EQ(ui::nice_bytes(33.0 * MiB), 64.0 * MiB);
  CHECK_EQ(ui::nice_bytes(64.0 * KiB), 64.0 * KiB);
  CHECK_EQ(ui::nice_bytes(0.0), 0.0);
  // Through the real axis-max path, with the Disk panel's 1 MiB/s floor.
  ui::PanelSpec p;
  p.id = "disk";
  p.y_scale.kind = ui::ScaleSpec::Kind::kBytesPerSec;
  p.y_scale.floor_ = 1.0 * MiB;
  ui::FrameState st;
  CHECK_EQ(ui::panel_y_max(p, 15.0 * MiB, st, Clock::now()), 16.0 * MiB);
}

TEST_CASE("absolute scale (RAM/VRAM): max is {1, 1.5} x 2^k above the total") {
  const double KiB = 1024.0, MiB = 1024.0 * KiB, GiB = 1024.0 * MiB;
  // The spec examples, plus exact hits on both sequence members.
  CHECK_EQ(ui::nice_bytes_max(22.8 * GiB), 24.0 * GiB);    // 1.5 x 2^4 GiB
  CHECK_EQ(ui::nice_bytes_max(15.4 * GiB), 16.0 * GiB);    // 1 x 2^4 GiB
  CHECK_EQ(ui::nice_bytes_max(4.0 * GiB), 4.0 * GiB);      // exact power of two
  CHECK_EQ(ui::nice_bytes_max(512.0 * MiB), 512.0 * MiB);  // exact power of two
  CHECK_EQ(ui::nice_bytes_max(12.0 * GiB), 12.0 * GiB);    // 1.5 x 2^3 GiB, exact
  CHECK_EQ(ui::nice_bytes_max(1.2 * GiB), 1.5 * GiB);      // smallest member above
  CHECK_EQ(ui::nice_bytes_max(24.1 * GiB), 32.0 * GiB);    // just above 24 -> next
  CHECK_EQ(ui::nice_bytes_max(0.0), 0.0);
  // Through the real axis-max path (Memory/VRAM panels use kAbsolute).
  ui::PanelSpec p;
  p.id = "mem";
  p.y_scale.kind = ui::ScaleSpec::Kind::kAbsolute;
  p.y_scale.absolute_max = 22.8 * GiB;
  ui::FrameState st;
  CHECK_EQ(ui::panel_y_max(p, 1.0, st, Clock::now()), 24.0 * GiB);
}

TEST_CASE("VRAM panel titles are '<GPU short> \xC2\xB7 Memory'") {
  const UiFixture fx;  // demo hardware: RTX 3050 Laptop dGPU + Cezanne iGPU
  const ui::PanelSpec* vram = nullptr;
  for (const auto& p : fx.page)
    if (p.id == "gpu.0000:01:00.0.vram") vram = &p;
  CHECK(vram != nullptr);
  if (vram) CHECK(vram->title == "RTX 3050 Laptop \xC2\xB7 Memory");
}

TEST_CASE("demo supplies GTT values for the iGPU") {
  DemoSource d;
  Snapshot s;
  d.probe();
  d.sample(s);
  CHECK_EQ(s.gpus.size(), static_cast<std::size_t>(2));
  CHECK(s.gpus[1].is_integrated);
  CHECK(s.gpus[1].gtt_used_bytes.has_value());
  CHECK(s.gpus[1].gtt_total_bytes.has_value());
  if (s.gpus[1].gtt_used_bytes && s.gpus[1].gtt_total_bytes)
    CHECK(*s.gpus[1].gtt_used_bytes <= *s.gpus[1].gtt_total_bytes);
}

TEST_CASE("layout cache: zero heap allocations when the size is unchanged") {
  // The Phase 3 spec: the layout is recomputed ONLY on a resize. A frame at an
  // unchanged terminal size must reuse the stored layout and allocate nothing
  // for it (the renderer's per-frame strings/vectors are a separate budget).
  const UiFixture fx;
  ui::LayoutCache cache;
  cache.page = &fx.page;
  cache.get(250, 70);  // one-time compute (allocates the layout vectors)

  geth::set_alloc_counting(true);
  const long long base = geth::alloc_count();
  for (int i = 0; i < 2000; ++i) cache.get(250, 70);  // same size: reuse
  CHECK_EQ(geth::alloc_count(), base);
  geth::set_alloc_counting(false);
}

TEST_CASE("fill cells are the derived fill of their OWN series color (all palette indices)") {
  // Phase 4 A1: every fill is derive_fill(series[color_index]) — a single
  // shared fill color was only right for the cyan series.
  const ui::Theme theme = ui::Theme::cyberpunk_blue();
  for (int i = 0; i < 6; ++i) {
    ftxui::Screen screen(138, 12);
    render_ramp(/*gap=*/false, screen, i);
    const int gx = 8, gy = 2, Wg = 129, Hg = 7;
    const ftxui::Color line_c =
        ui::to_ftxui(theme.series[i], ui::ColorMode::TrueColor);
    const ftxui::Color fill_c =
        ui::to_ftxui(ui::derive_fill(theme.series[i], theme.fill_mix),
                     ui::ColorMode::TrueColor);
    CHECK(line_c != fill_c);  // the theme actually derives a distinct fill
    bool saw_line = false, saw_fill = false;
    for (int ccol = 0; ccol < Wg; ++ccol)
      for (int cy = 0; cy < Hg; ++cy) {
        if (mask_at(screen, gx, gy, 2 * ccol, cy) == 0) continue;  // grid/empty
        const auto fg = screen.PixelAt(gx + ccol, gy + cy).foreground_color;
        // Every drawn graph cell is either the line or the derived fill of
        // THIS series' own color — nothing else.
        const bool ok = (fg == line_c) || (fg == fill_c);
        CHECK(ok);
        saw_line = saw_line || fg == line_c;
        saw_fill = saw_fill || fg == fill_c;
      }
    CHECK(saw_line);
    CHECK(saw_fill);
  }
}

TEST_CASE("256-color fills stay dark shades of their own series hue") {
  // Phase 4 A1 regression: on a 256-color terminal the green fill quantized
  // onto the border's teal (0,95,95) and the pink fill onto a grey ramp
  // color. fill_color() must pick a palette color of the series' own hue
  // family (within 30 degrees), and never the border's index for a
  // series whose fill is not the border's hue.
  const ui::Theme theme = ui::Theme::cyberpunk_blue();
  const int border_idx = ui::nearest256(theme.border);
  for (int i = 0; i < 6; ++i) {
    const int idx =
        fg256(ui::fill_color(theme.series[i], ui::ColorMode::Color256, theme.fill_mix));
    REQUIRE(idx >= 0);
    const double hs = ui::hue_deg(theme.series[i]);
    const double hp = ui::hue_deg(pal256_rgb(idx));
    REQUIRE(hp >= 0.0);  // never a grey ramp color for a colored series
    double dh = std::fabs(hs - hp);
    if (dh > 180.0) dh = 360.0 - dh;
    CHECK(dh <= 30.0);
    // And it is darker than the line (a shade, not the line color itself).
    const auto pal = pal256_rgb(idx);
    CHECK(pal.r + pal.g + pal.b <
          pal256_rgb(ui::nearest256(theme.series[i])).r +
          pal256_rgb(ui::nearest256(theme.series[i])).g +
          pal256_rgb(ui::nearest256(theme.series[i])).b);
    if (i != 0) CHECK(idx != border_idx);  // cyan's fill IS the border's teal
  }
}

TEST_CASE("same-device second series uses the lighter 50%-white variant") {
  // Phase 4 A2: VRAM + GTT — one device, one base color; the GTT series
  // (variant) draws in mix_white(base) so the legend bars visibly differ.
  const ui::Theme theme = ui::Theme::cyberpunk_blue();
  const ftxui::Color base_c =
      ui::to_ftxui(theme.series[2], ui::ColorMode::TrueColor);
  const ftxui::Color var_c =
      ui::to_ftxui(ui::mix_white(theme.series[2]), ui::ColorMode::TrueColor);
  CHECK(base_c != var_c);

  // A single panel with the iGPU's two VRAM-series shapes: VRAM (base) and
  // GTT (variant), like build_default_page emits for Cezanne (ci = 2).
  {
    const UiFixture fx;  // real demo data: the iGPU reports GTT
    const ui::PanelSpec* vram = nullptr;
    for (const auto& p : fx.page)
      if (p.id == "gpu.0000:07:00.0.vram") vram = &p;
    REQUIRE(vram != nullptr);
    REQUIRE_EQ(vram->series.size(), static_cast<std::size_t>(2));
    CHECK_EQ(vram->series[0].color_index, 2);
    CHECK(!vram->series[0].variant);
    CHECK_EQ(vram->series[1].color_index, 2);  // same device -> same base
    CHECK(vram->series[1].variant);
  }

  // Rendered: the two legend bars ("▌") are base and variant, in order.
  Collector c;
  c.add(std::make_unique<DemoSource>());
  c.probe_all(/*warm_up=*/true);
  for (int i = 0; i < 70; ++i) c.sample();
  ui::Hardware hw;
  hw.cpu_name = "Test CPU";
  ui::PanelSpec p;
  p.id = "vram";
  p.title = "T \xC2\xB7 Memory";
  p.series.push_back({.id = SeriesId("gpu.0000:07:00.0.vram"), .label = "VRAM",
                      .color_index = 2, .fill = true,
                      .value_fn = [](const Snapshot&) { return std::string("1 GiB"); }});
  p.series.push_back({.id = SeriesId("gpu.0000:07:00.0.gtt"), .label = "GTT",
                      .color_index = 2, .variant = true,
                      .value_fn = [](const Snapshot&) { return std::string("128 MiB"); }});
  std::vector<ui::PanelSpec> page{p};
  ui::FrameState state;
  ui::LayoutCache cache = cache_for(page);
  ftxui::Screen screen(138, 12);
  screen.Clear();
  ui::render_frame(screen, hw, page, c.store(), c.latest(), nullptr, state,
                   theme, ui::ColorMode::TrueColor, /*paused=*/false, 1000,
                   cache, 0, kWinNs);
  // Legend row = gy + Hg = 2 + 7 = 9.
  int bars[2];
  int found = 0;
  for (int x = 0; x < 138 && found < 2; ++x)
    if (screen.at(x, 9) == "\xE2\x96\x8C")  // ▌ U+258C
      bars[found++] = x;
  REQUIRE_EQ(found, 2);
  CHECK_EQ(screen.PixelAt(bars[0], 9).foreground_color, base_c);
  CHECK_EQ(screen.PixelAt(bars[1], 9).foreground_color, var_c);
}

TEST_CASE("0->max->0 spike over 3 samples: unbroken line on both slopes") {
  // Phase 4 A3: consecutive samples are connected in dot space (Bresenham),
  // so a steep rise/fall fills an unbroken vertical run of dots instead of
  // leaving isolated dots with gaps.
  ftxui::Screen screen(138, 12);
  render_spike(screen);
  const int gx = 8, gy = 2, Wg = 129, Hg = 7;
  const int dot_cols = 2 * Wg, dot_rows = 4 * Hg;
  const int bottom = dot_rows - 1;

  // Before the spike (~column 248) the line sits at 0: a single bottom dot.
  for (int xc = 0; xc <= 246; ++xc) {
    CHECK_EQ(top_dot(screen, gx, gy, xc, Hg), bottom);
    CHECK_EQ(bottom_dot(screen, gx, gy, xc, Hg), bottom);
  }
  // The spike region: every column's dots form one contiguous run, and
  // adjacent columns' runs touch (no vertical gap between them) — both
  // slopes.
  int deep_cols = 0;
  for (int xc = 247; xc < dot_cols; ++xc) {
    const int t = top_dot(screen, gx, gy, xc, Hg);
    const int b = bottom_dot(screen, gx, gy, xc, Hg);
    REQUIRE(t >= 0);  // data all the way to the window end
    CHECK(column_contiguous(screen, gx, gy, xc, Hg));
    if (b - t >= 2) ++deep_cols;  // a real vertical run (not a single dot)
    if (xc > 247) {
      const int pt = top_dot(screen, gx, gy, xc - 1, Hg);
      const int pb = bottom_dot(screen, gx, gy, xc - 1, Hg);
      CHECK(t <= pb + 1);  // the runs overlap or are vertically adjacent
      CHECK(pt <= b + 1);
    }
  }
  // Both slopes are steep enough to contain multi-dot vertical runs.
  CHECK(deep_cols >= 4);
  // The peak is in the middle of the spike (the 100 % sample maps to
  // ~column 252), and the line ends back at 0.
  int best = -1, best_xc = -1;
  for (int xc = 248; xc < dot_cols; ++xc)
    if (best < 0 || top_dot(screen, gx, gy, xc, Hg) < best) {
      best = top_dot(screen, gx, gy, xc, Hg);
      best_xc = xc;
    }
  CHECK(best >= 0);
  CHECK(best <= 2);  // the 100 % sample nearly reaches the top
  CHECK(best_xc >= 250);
  CHECK(best_xc <= 256);
  // The last column's run ends at 0 (its bottom dot is the 0 % sample); the
  // run's TOP is higher because the falling slope's vertical run enters it.
  CHECK_EQ(bottom_dot(screen, gx, gy, dot_cols - 1, Hg), bottom);
}

TEST_CASE("watts axis: ceiling's quarter is nice, minimum 20 W, no .5 labels") {
  // Phase 4 A4: the ceiling must be {4, 8, 10, 20} x 10^n so that the tick
  // labels (0/25/50/75/100 % of the ceiling) are whole watts.
  CHECK_EQ(ui::nice_watts(1.0), 20.0);    // below the floor -> the floor
  CHECK_EQ(ui::nice_watts(19.0), 20.0);
  CHECK_EQ(ui::nice_watts(20.0), 20.0);   // exact floor
  CHECK_EQ(ui::nice_watts(21.0), 40.0);
  CHECK_EQ(ui::nice_watts(40.0), 40.0);
  CHECK_EQ(ui::nice_watts(41.0), 80.0);
  CHECK_EQ(ui::nice_watts(80.0), 80.0);
  CHECK_EQ(ui::nice_watts(81.0), 100.0);
  CHECK_EQ(ui::nice_watts(100.0), 100.0);
  CHECK_EQ(ui::nice_watts(101.0), 200.0);
  CHECK_EQ(ui::nice_watts(200.0), 200.0);
  CHECK_EQ(ui::nice_watts(201.0), 400.0);
  CHECK_EQ(ui::nice_watts(555.0), 800.0);
  CHECK_EQ(ui::nice_watts(1000.0), 1000.0);
  // The ceiling's quarter is a nice number {1, 2, 2.5, 5} x 10^n.
  for (double m : {20.0, 40.0, 80.0, 100.0, 200.0, 400.0, 800.0, 1000.0, 2000.0}) {
    const double q = m / 4.0;
    const double p = std::pow(10.0, std::floor(std::log10(q)));
    const double r = q / p;
    const double nice[] = {1.0, 2.0, 2.5, 5.0};
    bool in_set = false;
    for (double n : nice)
      if (std::abs(r - n) < 1e-9) in_set = true;
    CHECK(in_set);
  }
  // Tick labels 0/25/50/75/100 % of the ceiling: never a decimal at all
  // (hence never a ".5").
  for (double m : {20.0, 40.0, 80.0, 100.0, 200.0, 400.0, 800.0, 1000.0, 4000.0})
    for (double f : {0.0, 0.25, 0.5, 0.75, 1.0})
      CHECK(format::compact_watts(m * f).find('.') == std::string::npos);
  // Through the real axis-max path, with the Power panel's 20 W floor.
  ui::PanelSpec p;
  p.id = "power";
  p.y_scale.kind = ui::ScaleSpec::Kind::kWatts;
  p.y_scale.floor_ = 20.0;
  ui::FrameState st;
  CHECK_EQ(ui::panel_y_max(p, 15.0, st, Clock::now()), 20.0);
  CHECK_EQ(ui::panel_y_max(p, 105.0, st, Clock::now()), 200.0);
}

TEST_CASE("byte-rate axis: quarter of the ceiling has no .5 label") {
  // Phase 4 A4: the byte-rate ceilings are {1,2,4,...,128} x 1024^n, so a
  // quarter is an exact power-of-two byte count — whole in whatever unit
  // the compact formatter picks (the spec notes this axis already holds).
  const double KiB = 1024.0, MiB = 1024.0 * KiB;
  for (double v : {1.0 * KiB, 64.0 * KiB, 0.5 * MiB, 15.0 * MiB, 100.0 * MiB,
                   5.0 * MiB, 33.0 * MiB, 1.0 * MiB}) {
    const double m = ui::nice_bytes(v);
    for (double f : {0.25, 0.5, 0.75, 1.0})
      CHECK(format::compact_rate(m * f).find('.') == std::string::npos);
  }
  // And with the per-panel floors the axis actually uses.
  ui::PanelSpec p;
  p.id = "net";
  p.y_scale.kind = ui::ScaleSpec::Kind::kBytesPerSec;
  p.y_scale.floor_ = 64.0 * KiB;
  ui::FrameState st;
  for (double m : {64.0 * KiB, 128.0 * KiB, 256.0 * KiB, 512.0 * KiB, 1.0 * MiB})
    for (double f : {0.25, 0.5, 0.75, 1.0})
      CHECK(format::compact_rate(m * f).find('.') == std::string::npos);
  CHECK_EQ(ui::panel_y_max(p, 1.0 * KiB, st, Clock::now()), 64.0 * KiB);
}

TEST_CASE("solid mode paints the theme background; transparent never does "
          "(every built-in theme, all three color modes)") {
  UiFixture fx;
  const ui::ColorMode modes[] = {ui::ColorMode::TrueColor, ui::ColorMode::Color256,
                                 ui::ColorMode::Color16};
  for (const auto& b : ui::builtin_themes()) {
    for (auto mode : modes) {
      // Solid: the single sanctioned background path must actually paint.
      {
        ui::FrameState st;
        ui::LayoutCache cache = cache_for(fx.page);
        ftxui::Screen screen(160, 48);
        screen.Clear();
        ui::FrameChrome ch;
        ch.solid_bg = true;
        ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr,
                         st, b.theme, mode, /*paused=*/false, 1000, cache, 0, kWinNs,
                         ch);
        CHECK_GT(count_background_codes(screen.ToString()), 0);
      }
      // Transparent (the default): no background is ever painted.
      {
        ui::FrameState st;
        ui::LayoutCache cache = cache_for(fx.page);
        ftxui::Screen screen(160, 48);
        screen.Clear();
        ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr,
                         st, b.theme, mode, /*paused=*/false, 1000, cache, 0, kWinNs);
        CHECK_EQ(count_background_codes(screen.ToString()), 0);
      }
    }
  }
}

TEST_CASE("header: [general].title overrides the auto title; without it, the auto title") {
  UiFixture fx;
  const std::string custom = "MY CUSTOM HEADER TITLE";
  for (bool use_override : {true, false}) {
    ui::FrameState st;
    ui::LayoutCache cache = cache_for(fx.page);
    ftxui::Screen screen(160, 48);
    screen.Clear();
    ui::FrameChrome ch;
    if (use_override) ch.title_override = custom;
    ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr, st,
                     ui::Theme::cyberpunk_blue(), ui::ColorMode::TrueColor,
                     /*paused=*/false, 1000, cache, 0, kWinNs, ch);
    const std::string out = screen.ToString();
    if (use_override)
      CHECK(out.find(custom) != std::string::npos);
    else
      CHECK(out.find(custom) == std::string::npos);
  }
}

}  // TEST_SUITE
