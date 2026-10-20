// UI tests: the transparency guarantee (GETH never paints a background color)
// and the "never crash at any terminal size" requirement. These drive the real
// render_frame() through an offscreen ftxui::Screen and inspect the SGR output.

#include <doctest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "core/collector.hpp"
#include "fake_pci_tree.hpp"
#include "sources/cpu_power.hpp"
#include "sources/demo.hpp"
#include "sources/gpu_registry.hpp"
#include "ui/frame.hpp"
#include "ui/panel_spec.hpp"
#include "ui/theme.hpp"
#include "util/alloc_count.hpp"
#include "util/format.hpp"

#include "ui_test_util.hpp"

#include "ftxui/screen/terminal.hpp"

using namespace geth;

namespace {

// FTXUI's Color constructors downgrade to the nearest lower-palette color when
// the detected terminal (Terminal::ColorSupport(), from COLORTERM/TERM) doesn't
// support the requested mode. The color-mode tests assert on the exact SGR form
// (38;2; / 38;5;), so they must be deterministic regardless of the build shell
// (an SSH/CI shell may have no COLORTERM). Pin the support to TrueColor for the
// whole test process. Test-only: it does not affect the app.
struct ForceTrueColorSupport {
  ForceTrueColorSupport() {
    ftxui::Terminal::SetColorSupport(ftxui::Terminal::Color::TrueColor);
  }
};
ForceTrueColorSupport kForceTrueColorSupport;

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
void render_ramp(bool gap, ftxui::Screen& screen, int color_index = 0,
                 bool pixels = false) {
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
  p.series.push_back(sr);
  std::vector<ui::PanelSpec> page{p};
  ui::FrameState state;
  ui::LayoutCache cache = cache_for(page);
  ui::FrameChrome chrome;
  chrome.pixels = pixels;
  ui::render_frame(screen, hw, page, c.store(), c.latest(), nullptr, state,
                   ui::Theme::cyberpunk_blue(), ui::ColorMode::TrueColor,
                   /*paused=*/false, 1000, cache, 0, kWinNs, chrome);
}

// A source that holds CPU usage at 0 % and spikes to 100 % for ONE sample in
// the second-to-last second of the 60 s window: ...0, 0, 0(t-2s), 100(t-1s),
// 0(now). The two slopes span only ~5 dot columns each: 27 dot rows in ~5
// columns, i.e. genuinely steep (several dot rows per column). It also holds
// RAM usage at a constant 0, so the spike line can share a two-series panel
// with a flat baseline line (text mode draws multi-series panels as one-dot
// braille — the flat line is what the continuity assertions anchor on).
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
      out.mem.ram_used_bytes = 0.0;
      return;
    }
    // Recorded samples run n = 0..59 (the probe does not increment n), so
    // n == 58 is the 59th of the 60 samples: 100 % one second before the end.
    out.cpu.usage_pct = (n == 58) ? 100.0 : 0.0;
    out.mem.ram_used_bytes = 0.0;
    ++n;
  }
  int n = 0;
  bool started = false;
};

// Renders the two-series panel the spike test needs: a flat RAM line (0 on a
// 128-unit absolute scale, dot row 27 = the bottom) and the CPU spike (the
// 100 % sample maps to 100/128 of the scale -> dot row 6). Two series ->
// one-dot braille, no fill; the flat line is the PRIMARY (series 0).
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
  p.id = "mem";
  p.title = "Mem & CPU";
  ui::SeriesSpec flat;
  flat.id = "mem.ram_used";
  flat.label = "RAM";
  flat.color_index = 0;
  p.series.push_back(flat);
  ui::SeriesSpec spike;
  spike.id = "cpu.usage";
  spike.label = "CPU";
  spike.color_index = 1;
  p.series.push_back(spike);
  // 128-unit absolute scale (nice_bytes_max(128) = 128): the flat line sits
  // at the bottom dot row, the 100 spike sample at 100/128 of the scale.
  // Tick labels "0".."128 B" -> L = 5, the same geometry as the 100 % panel.
  p.y_scale.kind = ui::ScaleSpec::Kind::kAbsolute;
  p.y_scale.absolute_max = 128.0;
  std::vector<ui::PanelSpec> page{p};
  ui::FrameState state;
  ui::LayoutCache cache = cache_for(page);
  ui::render_frame(screen, hw, page, c.store(), c.latest(), nullptr, state,
                   ui::Theme::cyberpunk_blue(), ui::ColorMode::TrueColor,
                   /*paused=*/false, 1000, cache, 0, kWinNs);
}

// A source that holds CPU usage at a constant 45 % of the 128-unit scale
// (57.6) and RAM at a constant 50 % of it (64) — two flat horizontal lines
// one dot row apart (14 and 15), which land in the SAME braille char row
// (3) in every column: the "primary wins the shared cell" assertion needs
// two series writing different dots into one cell.
struct ConstSource : Source {
  const char* name() const override { return "const (test)"; }
  SourceStatus probe() override {
    SourceStatus st;
    st.available = true;
    st.description = name();
    return st;
  }
  void sample(Snapshot& out) override {
    out.cpu.usage_pct = 57.6;
    out.mem.ram_used_bytes = 64.0;
  }
};

// Renders the two-constant-lines panel: series 0 = RAM at 64/128 = 50 % of
// the scale (dot row 14, PRIMARY) and series 1 = CPU at 57.6/128 = 45 %
// (dot row 15). Two series -> one-dot braille, no fill. 57.6/64 were picked
// so the two rows are one apart: row 14 is braille row 2, row 15 row 3 of
// char row 3, so EVERY cell of char row 3 holds both dots — the even dot
// column's bits 2+6 and the odd column's bits 5+7 both live in the same
// cell, whose full mask is (1<<2)|(1<<5)|(1<<6)|(1<<7) = 228.
void render_two_const(ftxui::Screen& screen) {
  Collector c;
  c.add(std::make_unique<ConstSource>());
  c.probe_all(/*warm_up=*/true);
  TimePoint vnow = c.latest().time;
  for (int i = 1; i <= 60; ++i) {
    vnow += std::chrono::seconds(1);
    c.set_sample_time(vnow);
    c.sample();
  }
  ui::Hardware hw;
  hw.cpu_name = "Test CPU";
  ui::PanelSpec p;
  p.id = "mem";
  p.title = "Mem & CPU";
  ui::SeriesSpec ram;
  ram.id = "mem.ram_used";
  ram.label = "RAM";
  ram.color_index = 0;  // primary
  p.series.push_back(ram);
  ui::SeriesSpec cpu;
  cpu.id = "cpu.usage";
  cpu.label = "CPU";
  cpu.color_index = 1;
  p.series.push_back(cpu);
  p.y_scale.kind = ui::ScaleSpec::Kind::kAbsolute;
  p.y_scale.absolute_max = 128.0;
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

// The area chart's block glyphs: U+2581..U+2588 (▁..█); index 0 = no ink,
// 1..7 = ▁..▇, 8 = a full cell. Mirrors the renderer's kEighths table.
const char* kAreaGlyph(int eighth) {
  static const char* const k[9] = {
      " ", "\xE2\x96\x81", "\xE2\x96\x82", "\xE2\x96\x83", "\xE2\x96\x84",
      "\xE2\x96\x85", "\xE2\x96\x86", "\xE2\x96\x87", "\xE2\x96\x88"};
  return k[eighth];
}

// True for any of the area chart's glyphs (▁..█) — the block range U+2581-88.
bool is_area_glyph(const std::string& ch) {
  return ch.size() == 3 && static_cast<unsigned char>(ch[0]) == 0xE2 &&
         static_cast<unsigned char>(ch[1]) == 0x96 &&
         static_cast<unsigned char>(ch[2]) >= 0x81 &&
         static_cast<unsigned char>(ch[2]) <= 0x88;
}

// Bottom of the TOPMOST contiguous run of dots in dot column cc, or -1 when
// the column is empty (scans down from top_dot while the next row is set).
// With a second, lower line in the column, this tracks the TOP line's run
// without being broken by the lower line's dots.
int top_run_bottom(const ftxui::Screen& s, int gx, int gy, int cc, int Hg) {
  const int t = top_dot(s, gx, gy, cc, Hg);
  if (t < 0) return -1;
  int b = t;
  while (b + 1 < 4 * Hg &&
         (mask_at(s, gx, gy, cc, (b + 1) / 4) &
          (1u << ui::braille_bit(cc % 2, (b + 1) % 4))))
    ++b;
  return b;
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

// The color-mode checks render with an explicit ColorMode and assert on the
// exact SGR form (38;2; / 38;5;). FTXUI's Color constructors downgrade to the
// nearest lower-palette color when the detected terminal (Terminal::
// ColorSupport(), from COLORTERM/TERM) doesn't support the requested mode, so
// ForceTrueColorSupport pins the support to TrueColor for the whole test
// process, keeping these checks deterministic in any build shell. If a check
// still fails, dump what the frame actually contained (which SGRs survived, and
// what to_ftxui itself produced) so the first recurrence is diagnosable.
void diag_color_mode(const std::string& out, const ui::Theme& th) {
  const std::string tc = ui::to_ftxui(th.title, ui::ColorMode::TrueColor).Print(false);
  const std::string c256 = ui::to_ftxui(th.title, ui::ColorMode::Color256).Print(false);
  std::fprintf(stderr, "\ngeth: [diag] color-mode check failed; theme.title = #%02X%02X%02X\n",
               th.title.r, th.title.g, th.title.b);
  std::fprintf(stderr, "geth: [diag] to_ftxui(title): truecolor=\"%s\" 256=\"%s\"\n", tc.c_str(),
               c256.c_str());
  std::fprintf(stderr,
               "geth: [diag] frame: %zu bytes, has '38;2;': %s, has '38;5;': %s\n", out.size(),
               out.find("38;2;") != std::string::npos ? "yes" : "no",
               out.find("38;5;") != std::string::npos ? "yes" : "no");
  std::string esc;
  esc.reserve(600);
  for (std::size_t i = 0; i < out.size() && i < 400; ++i) {
    const unsigned char ch = static_cast<unsigned char>(out[i]);
    if (ch == 0x1b)
      esc += "\\x1b";
    else if (ch < 32) {
      char b[8];
      std::snprintf(b, sizeof b, "\\x%02x", ch);
      esc += b;
    } else
      esc += static_cast<char>(ch);
  }
  std::fprintf(stderr, "geth: [diag] frame head: %s\n", esc.c_str());
}

}  // namespace

TEST_SUITE("ui") {

TEST_CASE("no background color is ever emitted (160x48, all modes, both layers)") {
  const UiFixture fx;
  const ui::Theme theme = ui::Theme::cyberpunk_blue();
  const ui::ColorMode modes[] = {ui::ColorMode::TrueColor, ui::ColorMode::Color256,
                                 ui::ColorMode::Color16};
  // The transparency guarantee holds in the TEXT layer (area chart + braille,
  // both foreground-only) and in the PIXELS text layer (graphs left blank —
  // the pixel backend paints them).
  for (auto mode : modes)
    for (bool pixels : {false, true}) {
      ui::FrameState state;
      ui::LayoutCache cache = cache_for(fx.page);
      ftxui::Screen screen(160, 48);
      screen.Clear();
      ui::FrameChrome chrome;
      chrome.pixels = pixels;
      ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr,
                       state, theme, mode, /*paused=*/false, 1000, cache, 0, kWinNs,
                       chrome);
      const std::string out = screen.ToString();
      // Sanity: a frame was actually drawn (borders, title, footer).
      CHECK(!out.empty());
      CHECK(out.find('\xe2') != std::string::npos);  // some UTF-8 (box/block) glyph
      CHECK(!has_forbidden_style(out));  // no background SGR in either layer
    }
}

// v1.3.0 steps mode (nvtop-style step-line history chart).
//
// A flat run is "─"; a value change is a vertical segment of "│" between two
// square corners. row 0 = top, row Hg-1 = bottom; value = vmax -> top, 0 ->
// bottom. No fill, no braille. Inspired by nvtop (GPLv3); reimplemented
// independently for GETH (MIT) — no nvtop code is copied.

// A RawWindow view over test-owned sample vectors (time-ordered).
ui::RawWindow make_raw(const std::vector<int64_t>& t, const std::vector<float>& v,
                       int64_t t_start, int64_t t_end) {
  ui::RawWindow w;
  w.t = t.data();
  w.v = v.data();
  w.n = t.size();
  w.t_start = t_start;
  w.t_end = t_end;
  w.span_ns = static_cast<double>(t_end - t_start);
  return w;
}

// The glyph at graph-local cell (cc, rr) of a screen whose graph rect starts
// at (gx, gy). A cell draw_step_chart did not touch is "" (after Clear()).
std::string glyph_at(const ftxui::Screen& s, int gx, int gy, int cc, int rr) {
  if (cc < 0 || rr < 0) return "";
  return s.PixelAt(gx + cc, gy + rr).character;
}

TEST_CASE("steps: step_row maps value -> row (top = 0, clamped)") {
  // Hg = 11 rows, vmax = 100: row = round((1 - v/100) * 10).
  CHECK_EQ(ui::step_row(100.0, 100.0, 11), 0);   // max -> top
  CHECK_EQ(ui::step_row(0.0, 100.0, 11), 10);    // 0 -> bottom
  CHECK_EQ(ui::step_row(30.0, 100.0, 11), 7);    // (1 - 0.3) * 10 = 7
  CHECK_EQ(ui::step_row(70.0, 100.0, 11), 3);    // (1 - 0.7) * 10 = 3
  CHECK_EQ(ui::step_row(50.0, 100.0, 11), 5);    // (1 - 0.5) * 10 = 5
  // Out-of-range values are clamped into the graph rectangle.
  CHECK_EQ(ui::step_row(150.0, 100.0, 11), 0);   // above vmax -> top
  CHECK_EQ(ui::step_row(-20.0, 100.0, 11), 10);  // below 0 -> bottom
  // Degenerate: value == vmax -> top; vmax <= 0 -> bottom; one row -> 0.
  CHECK_EQ(ui::step_row(5.0, 5.0, 11), 0);
  CHECK_EQ(ui::step_row(5.0, 0.0, 11), 10);
  CHECK_EQ(ui::step_row(5.0, 100.0, 1), 0);
}

TEST_CASE("steps: step_column_values is the mean of the in-slice samples") {
  // Wg = 2, window 20 s: column 0 = [0, 10 s), column 1 = [10 s, 20 s).
  const int64_t S = 1000000000LL;
  std::vector<int64_t> t = {2 * S, 6 * S, 12 * S, 16 * S};
  std::vector<float> v = {10.0f, 30.0f, 50.0f, 70.0f};
  ui::RawWindow w = make_raw(t, v, 2 * S, 20 * S);
  ui::Decimated d;  // empty: every slice holds a sample, value_at is unused
  const auto vals = ui::step_column_values(w, d, 2, 20 * S, 20 * S);
  CHECK_EQ(vals.size(), 2u);
  CHECK_EQ(vals[0], 20.0);  // mean(10, 30)
  CHECK_EQ(vals[1], 60.0);  // mean(50, 70)
}

TEST_CASE("steps: an empty slice falls back to value_at at its midpoint") {
  const int64_t S = 1000000000LL;
  // Column 0 = [0,10 s) holds one sample; column 1 = [10,20 s) holds none, so
  // it interpolates the decimated line at the slice midpoint (15 s).
  std::vector<int64_t> t = {2 * S};
  std::vector<float> v = {10.0f};
  ui::RawWindow w = make_raw(t, v, 2 * S, 20 * S);
  ui::Decimated d;
  d.t = {10 * S, 20 * S};
  d.v = {20.0f, 40.0f};  // value_at(15 s) = 20 + (40 - 20) * 0.5 = 30
  const auto vals = ui::step_column_values(w, d, 2, 20 * S, 20 * S);
  CHECK_EQ(vals.size(), 2u);
  CHECK_EQ(vals[0], 10.0);  // the lone in-slice sample
  CHECK_EQ(vals[1], 30.0);  // interpolated at the slice midpoint
}

TEST_CASE("steps: an all-NaN slice is a gap (NaN); the next slice resumes") {
  const int64_t S = 1000000000LL;
  // Column 0 = [0,10 s) holds only NaN samples -> NaN (a gap). Column 1 =
  // [10,20 s) holds a finite sample -> its value.
  std::vector<int64_t> t = {2 * S, 12 * S};
  std::vector<float> v = {std::nan(""), 50.0f};
  ui::RawWindow w = make_raw(t, v, 2 * S, 20 * S);
  ui::Decimated d;
  const auto vals = ui::step_column_values(w, d, 2, 20 * S, 20 * S);
  CHECK_EQ(vals.size(), 2u);
  CHECK(std::isnan(vals[0]));  // gap: no line in this column
  CHECK_EQ(vals[1], 50.0);
}

TEST_CASE("steps: a constant series draws a flat line across the full width") {
  const int Wg = 10, Hg = 11;
  ftxui::Screen screen(40, 40);
  screen.Clear();
  const int gx = 5, gy = 5;
  // Constant value 70 -> row 3 ((1 - 0.7) * 10 = 3).
  std::vector<double> cols(Wg, 70.0);
  const std::vector<ftxui::Color> colors = {ftxui::Color::Red};
  const std::vector<int> order = {0};
  ui::draw_step_chart(screen, gx, gy, Wg, Hg, {cols}, colors, order,
                      /*vmax=*/100.0, {}, ftxui::Color::GrayDark);
  // Every column is a horizontal on row 3; every other row is empty.
  for (int c = 0; c < Wg; ++c)
    CHECK_EQ(glyph_at(screen, gx, gy, c, 3), "\xE2\x94\x80");  // ─
  for (int c = 0; c < Wg; ++c)
    for (int r = 0; r < Hg; ++r)
      if (r != 3) CHECK_EQ(glyph_at(screen, gx, gy, c, r), "");
}

TEST_CASE("steps: a step up draws ┘ at the old (lower) row, ┌ at the new (upper), │ between") {
  const int Wg = 10, Hg = 11;
  ftxui::Screen screen(40, 40);
  screen.Clear();
  const int gx = 5, gy = 5;
  // Columns 0-4 = 30 (row 7), columns 5-9 = 70 (row 3). The step is at c = 5.
  std::vector<double> cols(Wg, 70.0);
  for (int c = 0; c < 5; ++c) cols[c] = 30.0;
  const std::vector<ftxui::Color> colors = {ftxui::Color::Red};
  const std::vector<int> order = {0};
  ui::draw_step_chart(screen, gx, gy, Wg, Hg, {cols}, colors, order,
                      /*vmax=*/100.0, {}, ftxui::Color::GrayDark);
  // The old (lower) run: horizontal on row 7 up to the step column.
  for (int c = 0; c < 5; ++c)
    CHECK_EQ(glyph_at(screen, gx, gy, c, 7), "\xE2\x94\x80");  // ─
  // The step column: ┘ at row 7 (old/lower), │ at rows 4-6, ┌ at row 3 (new/upper).
  CHECK_EQ(glyph_at(screen, gx, gy, 5, 7), "\xE2\x94\x98");  // ┘
  CHECK_EQ(glyph_at(screen, gx, gy, 5, 6), "\xE2\x94\x82");  // │
  CHECK_EQ(glyph_at(screen, gx, gy, 5, 5), "\xE2\x94\x82");  // │
  CHECK_EQ(glyph_at(screen, gx, gy, 5, 4), "\xE2\x94\x82");  // │
  CHECK_EQ(glyph_at(screen, gx, gy, 5, 3), "\xE2\x94\x8C");  // ┌
  // The new (upper) run: horizontal on row 3 after the step.
  for (int c = 6; c < Wg; ++c)
    CHECK_EQ(glyph_at(screen, gx, gy, c, 3), "\xE2\x94\x80");  // ─
  // Nothing else is drawn (no gridlines given; rows 2 and 8 stay empty).
  CHECK_EQ(glyph_at(screen, gx, gy, 5, 2), "");
  CHECK_EQ(glyph_at(screen, gx, gy, 5, 8), "");
}

TEST_CASE("steps: a step down draws ┐ at the old (upper) row, └ at the new (lower), │ between") {
  const int Wg = 10, Hg = 11;
  ftxui::Screen screen(40, 40);
  screen.Clear();
  const int gx = 5, gy = 5;
  // Columns 0-4 = 70 (row 3), columns 5-9 = 30 (row 7). The step is at c = 5.
  std::vector<double> cols(Wg, 30.0);
  for (int c = 0; c < 5; ++c) cols[c] = 70.0;
  const std::vector<ftxui::Color> colors = {ftxui::Color::Red};
  const std::vector<int> order = {0};
  ui::draw_step_chart(screen, gx, gy, Wg, Hg, {cols}, colors, order,
                      /*vmax=*/100.0, {}, ftxui::Color::GrayDark);
  // The old (upper) run: horizontal on row 3 up to the step column.
  for (int c = 0; c < 5; ++c)
    CHECK_EQ(glyph_at(screen, gx, gy, c, 3), "\xE2\x94\x80");  // ─
  // The step column: ┐ at row 3 (old/upper), │ at rows 4-6, └ at row 7 (new/lower).
  CHECK_EQ(glyph_at(screen, gx, gy, 5, 3), "\xE2\x94\x90");  // ┐
  CHECK_EQ(glyph_at(screen, gx, gy, 5, 4), "\xE2\x94\x82");  // │
  CHECK_EQ(glyph_at(screen, gx, gy, 5, 5), "\xE2\x94\x82");  // │
  CHECK_EQ(glyph_at(screen, gx, gy, 5, 6), "\xE2\x94\x82");  // │
  CHECK_EQ(glyph_at(screen, gx, gy, 5, 7), "\xE2\x94\x94");  // └
  // The new (lower) run: horizontal on row 7 after the step.
  for (int c = 6; c < Wg; ++c)
    CHECK_EQ(glyph_at(screen, gx, gy, c, 7), "\xE2\x94\x80");  // ─
}

TEST_CASE("steps: the first column and a post-gap restart are ─, never a corner") {
  const int Wg = 8, Hg = 11;
  ftxui::Screen screen(40, 40);
  screen.Clear();
  const int gx = 5, gy = 5;
  // Columns 0-2 = 30 (row 7), column 3 = NaN (gap), columns 4-7 = 30 (row 7).
  const std::vector<double> cols = {30.0, 30.0, 30.0, std::nan(""),
                                    30.0, 30.0, 30.0, 30.0};
  const std::vector<ftxui::Color> colors = {ftxui::Color::Red};
  const std::vector<int> order = {0};
  ui::draw_step_chart(screen, gx, gy, Wg, Hg, {cols}, colors, order,
                      /*vmax=*/100.0, {}, ftxui::Color::GrayDark);
  // The first column is a plain horizontal (a corner never comes from nothing).
  CHECK_EQ(glyph_at(screen, gx, gy, 0, 7), "\xE2\x94\x80");  // ─
  // The gap column is entirely empty.
  for (int r = 0; r < Hg; ++r) CHECK_EQ(glyph_at(screen, gx, gy, 3, r), "");
  // The restart after the gap is a plain horizontal again (no corner).
  for (int c = 4; c < Wg; ++c)
    CHECK_EQ(glyph_at(screen, gx, gy, c, 7), "\xE2\x94\x80");  // ─
}

TEST_CASE("steps: a horizontal crossing a vertical takes ┼ / ┴ / ┬ in the vertical's color") {
  const int Wg = 8, Hg = 11;
  // Series 0 (red) steps UP at column 4: row 7 -> row 3 (a vertical spanning
  // rows 3..7). Series 1 (green) is a horizontal that crosses it.
  std::vector<double> a(Wg, 70.0);  // 70 -> row 3
  for (int c = 0; c < 4; ++c) a[c] = 30.0;  // 30 -> row 7
  const std::vector<ftxui::Color> colors = {ftxui::Color::Red, ftxui::Color::Green};
  const std::vector<int> order = {0, 1};

  // (a) Crossing the middle of the segment: green at row 5 -> ┼, in RED.
  {
    ftxui::Screen screen(40, 40);
    screen.Clear();
    const int gx = 5, gy = 5;
    const std::vector<double> b(Wg, 50.0);  // 50 -> row 5 (inside 3..7)
    ui::draw_step_chart(screen, gx, gy, Wg, Hg, {a, b}, colors, order,
                        /*vmax=*/100.0, {}, ftxui::Color::GrayDark);
    const ftxui::Pixel p = screen.PixelAt(gx + 4, gy + 5);
    CHECK_EQ(p.character, "\xE2\x94\xBC");  // ┼
    CHECK(p.foreground_color == ftxui::Color::Red);  // the vertical owner's color
  }
  // (b) Meeting the lower corner row: green at row 7 -> ┴, in RED.
  {
    ftxui::Screen screen(40, 40);
    screen.Clear();
    const int gx = 5, gy = 5;
    const std::vector<double> b(Wg, 30.0);  // 30 -> row 7 (= the lower corner)
    ui::draw_step_chart(screen, gx, gy, Wg, Hg, {a, b}, colors, order,
                        /*vmax=*/100.0, {}, ftxui::Color::GrayDark);
    const ftxui::Pixel p = screen.PixelAt(gx + 4, gy + 7);
    CHECK_EQ(p.character, "\xE2\x94\xB4");  // ┴
    CHECK(p.foreground_color == ftxui::Color::Red);
  }
  // (c) Meeting the upper corner row: green at row 3 -> ┬, in RED.
  {
    ftxui::Screen screen(40, 40);
    screen.Clear();
    const int gx = 5, gy = 5;
    const std::vector<double> b(Wg, 70.0);  // 70 -> row 3 (= the upper corner)
    ui::draw_step_chart(screen, gx, gy, Wg, Hg, {a, b}, colors, order,
                        /*vmax=*/100.0, {}, ftxui::Color::GrayDark);
    const ftxui::Pixel p = screen.PixelAt(gx + 4, gy + 3);
    CHECK_EQ(p.character, "\xE2\x94\xAC");  // ┬
    CHECK(p.foreground_color == ftxui::Color::Red);
  }
}

TEST_CASE("steps: gridlines (┈) fill only the cells no line occupies") {
  const int Wg = 8, Hg = 11;
  ftxui::Screen screen(40, 40);
  screen.Clear();
  const int gx = 5, gy = 5;
  // One series steps up at column 4, crossing the gridline row (5).
  std::vector<double> a(Wg, 70.0);  // 70 -> row 3
  for (int c = 0; c < 4; ++c) a[c] = 30.0;  // 30 -> row 7
  const std::vector<ftxui::Color> colors = {ftxui::Color::Red};
  const std::vector<int> order = {0};
  const std::vector<int> grid_rows = {5};
  ui::draw_step_chart(screen, gx, gy, Wg, Hg, {a}, colors, order,
                      /*vmax=*/100.0, grid_rows, ftxui::Color::GrayDark);
  // Row 5 is the gridline row: ┈ in every cell the line does not occupy, and
  // the line's own glyph (the vertical's │) where it does.
  CHECK_EQ(glyph_at(screen, gx, gy, 4, 5), "\xE2\x94\x82");  // │ (the vertical)
  for (int c = 0; c < Wg; ++c)
    if (c != 4) CHECK_EQ(glyph_at(screen, gx, gy, c, 5), "\xE2\x94\x88");  // ┈
  // A non-gridline, non-line row stays empty.
  for (int c = 0; c < Wg; ++c) CHECK_EQ(glyph_at(screen, gx, gy, c, 1), "");
}

// v1.3.0 steps mode: chrome.steps draws the step-line chart (text-rendered),
// keeps the geometry valid, and never emits a background SGR (every color mode).
TEST_CASE("steps: chrome.steps draws box glyphs, keeps geometry, no background SGR") {
  const UiFixture fx;
  const ui::Theme theme = ui::Theme::cyberpunk_blue();
  for (auto mode : {ui::ColorMode::TrueColor, ui::ColorMode::Color256,
                    ui::ColorMode::Color16}) {
    ui::FrameState state;
    ui::LayoutCache cache = cache_for(fx.page);
    ftxui::Screen screen(160, 48);
    screen.Clear();
    ui::FrameChrome chrome;
    chrome.steps = true;  // pixels stays false: steps are text-rendered
    ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr,
                     state, theme, mode, /*paused=*/false, 1000, cache, 0, kWinNs,
                     chrome);
    const std::string out = screen.ToString();
    CHECK(out.find('\xe2') != std::string::npos);  // some glyph drawn
    CHECK(!has_forbidden_style(out));  // no background SGR
    // Box-drawing glyphs (the U+2500 block) are present in the graph rects.
    CHECK(out.find("\xE2\x94\x80") != std::string::npos);  // ─
    // The geometry self-check: every line is exactly 160 display cols wide.
    const ui::GeometryReport rep = ui::check_geometry(ui::screen_to_text(screen), 160);
    CHECK_EQ(rep.lines, 48);
    CHECK(rep.bad_lines.empty());
  }
}

// v1.0.1 Part A.3: every BUILT-IN theme renders in all three color modes
// (paired with the 16-slot distinctness test in test_theme.cpp).
TEST_CASE("every built-in theme renders in truecolor, 256 and 16 colors") {
  const UiFixture fx;
  const ui::Theme themes[] = {ui::Theme::cyberpunk_blue(), ui::Theme::matrix_green(),
                              ui::Theme::crimson_red(), ui::Theme::amber(),
                              ui::Theme::blaze_orange()};
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
      if (mode == ui::ColorMode::TrueColor) {
        if (out.find("38;2;") == std::string::npos) diag_color_mode(out, theme);
        CHECK(out.find("38;2;") != std::string::npos);  // 24-bit fg present
      } else if (mode == ui::ColorMode::Color256) {
        if (out.find("38;5;") == std::string::npos) diag_color_mode(out, theme);
        CHECK(out.find("38;5;") != std::string::npos);  // 256-color fg present
      } else {
        if (out.find("38;5;") != std::string::npos) diag_color_mode(out, theme);
        CHECK(out.find("38;5;") == std::string::npos);  // 16: palette SGRs only
      }
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

TEST_CASE("area chart: a 0->100% ramp fills exact eighth heights, monotonic, gradient") {
  // Single-series panel -> the area chart. The stored samples run el = 1..60 s
  // (the probe sample is NOT recorded) with v(el) = min(100, 100el/59); the
  // window spans the whole history (t0+1s .. t0+60s, span 59 s), so character
  // column cc sits at el = 1 + 59cc/(Wg-1) and v(cc) = min(100,
  // 100(128+59cc)/7552). The decimated line is EXACTLY that ramp (linear
  // interpolation on a straight line), so the per-column heights are known.
  const ui::Theme theme = ui::Theme::cyberpunk_blue();
  for (int i = 0; i < 6; ++i) {
    ftxui::Screen screen(138, 12);
    render_ramp(/*gap=*/false, screen, i);
    const int gx = 8, gy = 2, Wg = 129, Hg = 7;
    int prev_top_b = -1;
    for (int cc = 0; cc < Wg; ++cc) {
      const double v = std::min(100.0, 100.0 * (128 + 59 * cc) / 7552.0);
      int h8 = std::lround(v / 100.0 * Hg * 8.0);
      h8 = std::clamp(h8, 0, Hg * 8);
      const int eighth = h8 % 8;
      const int top_b = h8 / 8 + (eighth > 0 ? 1 : 0) - 1;  // top row, bottom-index
      CHECK(top_b >= prev_top_b);  // the ramp never descends left -> right
      prev_top_b = top_b;
      REQUIRE(top_b >= 0);  // the ramp starts at ~1.7 %, never exactly 0
      // Top cell: the eighth glyph (a full block when the top row is whole).
      CHECK_EQ(screen.at(gx + cc, gy + Hg - 1 - top_b), kAreaGlyph(eighth > 0 ? eighth : 8));
      // Every row below the top: full blocks.
      for (int b = 0; b < top_b; ++b)
        CHECK_EQ(screen.at(gx + cc, gy + Hg - 1 - b), kAreaGlyph(8));
      // The per-column gradient, exact in truecolor: the bottom row is the
      // series color scaled to 0.45, the top row the full color, linear in
      // between (fill_color is the exact scale in TrueColor mode).
      for (int b = 0; b <= top_b; ++b) {
        const double f = (top_b > 0) ? 0.45 + 0.55 * b / (double)top_b : 1.0;
        const ui::Rgb want = ui::scale(theme.series[i], f);
        const ftxui::Color want_c = ftxui::Color::RGB(want.r, want.g, want.b);
        CHECK_EQ(screen.PixelAt(gx + cc, gy + Hg - 1 - b).foreground_color,
                 want_c);
      }
      // Above the area: no block glyph (at most the dim gridline).
      for (int cy = 0; cy < Hg - 1 - top_b; ++cy)
        CHECK_FALSE(is_area_glyph(screen.at(gx + cc, gy + cy)));
    }
  }
}

TEST_CASE("area chart: a NaN gap renders as empty columns, never 0") {
  ftxui::Screen screen(138, 12);
  render_ramp(/*gap=*/true, screen);
  const int gx = 8, gy = 2, Hg = 7;
  // Gap el 25..35 s of the 59 s span; column cc sits at el = 1 + 59cc/128, so
  // every column whose interpolation touches a NaN sample (cc 52..75) is
  // empty. The interior 55..72 is asserted; data stands on both sides.
  for (int cc = 55; cc <= 72; ++cc)
    for (int cy = 0; cy < Hg; ++cy)
      CHECK_FALSE(is_area_glyph(screen.at(gx + cc, gy + cy)));
  for (int cc : {40, 90}) {
    bool any = false;
    for (int cy = 0; cy < Hg; ++cy)
      any = any || is_area_glyph(screen.at(gx + cc, gy + cy));
    CHECK(any);
  }
}

TEST_CASE("multi-series: one dot per position, no fill, primary wins shared cells") {
  ftxui::Screen screen(138, 12);
  render_two_const(screen);
  const int gx = 8, gy = 2, Wg = 129, Hg = 7;
  // Both lines are flat: RAM 64/128 -> dot row 14, CPU 57.6/128 -> row 15.
  // Every dot column has EXACTLY those two dots and no others: one dot thick
  // (no 2-dot braille), no fill, and every other dot row of the graph empty
  // (a dotted fill would light the rows between and below the lines).
  for (int xc = 0; xc < 2 * Wg; ++xc)
    for (int dr = 0; dr < 4 * Hg; ++dr) {
      const bool set =
          mask_at(screen, gx, gy, xc, dr / 4) &
          (1u << ui::braille_bit(xc % 2, dr % 4));
      const bool want = (dr == 14) || (dr == 15);
      CHECK_EQ(static_cast<bool>(set), want);
    }
  // Both dots share char row 3 in every column (bits 2+6 for even dot
  // columns, 5+7 for odd) — and the cell's color is the PRIMARY series',
  // which draws last and overwrites the other series' color on the shared
  // cell. REQUIRE the colors differ so the check means something.
  const auto c0 = ui::to_ftxui(ui::Theme::cyberpunk_blue().series[0],
                               ui::ColorMode::TrueColor);
  const auto c1 = ui::to_ftxui(ui::Theme::cyberpunk_blue().series[1],
                               ui::ColorMode::TrueColor);
  REQUIRE(c0 != c1);
  for (int xc = 0; xc < 2 * Wg; ++xc) {
    // Each braille cell holds BOTH dot columns, so every char-row-3 cell
    // carries both lines: even column bits 2+6, odd column bits 5+7 ->
    // (1<<2)|(1<<5)|(1<<6)|(1<<7) = 228 in every cell.
    const std::uint8_t got_mask = mask_at(screen, gx, gy, xc, 3);
    CHECK_EQ(got_mask, 228);
    CHECK_EQ(screen.PixelAt(gx + xc / 2, gy + 3).foreground_color, c0);
  }
}

TEST_CASE("pixels layer: the text frame leaves the graph rect blank for the image") {
  // The pixel backend draws each graph image over exactly the text layer's
  // graph rect, so in pixels mode the text layer must leave that rectangle
  // untouched: no glyphs, no gridlines, no SGR — byte-for-byte the screen's
  // initial contents. Everything outside it (borders, titles, the y axis)
  // is still drawn as text.
  ftxui::Screen blank(138, 12);
  blank.Clear();
  ftxui::Screen screen(138, 12);
  screen.Clear();
  render_ramp(/*gap=*/false, screen, /*color_index=*/0, /*pixels=*/true);
  const int gx = 8, gy = 2, Wg = 129, Hg = 7;
  for (int x = gx; x < gx + Wg; ++x)
    for (int y = gy; y < gy + Hg; ++y) {
      // Character AND style untouched: space, default fg/bg (no SGR emitted).
      CHECK_EQ(screen.at(x, y), blank.at(x, y));
      CHECK_EQ(screen.PixelAt(x, y).foreground_color,
               blank.PixelAt(x, y).foreground_color);
      CHECK_EQ(screen.PixelAt(x, y).background_color,
               blank.PixelAt(x, y).background_color);
    }
  // The y-axis separator sits in the graph rect's left neighbour and keeps
  // "│" on every graph row (it is drawn in BOTH modes).
  for (int y = gy; y < gy + Hg; ++y) {
    const std::string& ch = screen.at(gx - 1, y);
    const bool is_axis = ch == "\xE2\x94\x82" || ch == "\xE2\x94\xA4";  // │ or ┤
    CHECK(is_axis);
  }
  // Contrast: the SAME frame in text mode is NOT blank — the area chart
  // stands in the rect.
  ftxui::Screen text(138, 12);
  text.Clear();
  render_ramp(/*gap=*/false, text, /*color_index=*/0, /*pixels=*/false);
  bool any = false;
  for (int x = gx; x < gx + Wg; ++x)
    for (int y = gy; y < gy + Hg; ++y)
      any = any || is_area_glyph(text.at(x, y));
  CHECK(any);
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
    // A series in the border's own hue family (within fill_color's 30-degree
    // constraint) may legitimately share the border's 256 index; every other
    // series must not. (The old special case "i != 0" was the cyan slot 1;
    // the new slot 5 is slot 1 mixed 40 % toward white, so it is in the same
    // teal family and is covered by the hue check too.)
    const double hb = ui::hue_deg(theme.border);
    const double dfam = (hs >= 0.0 && hb >= 0.0)
                            ? std::min(std::fabs(hs - hb), 360.0 - std::fabs(hs - hb))
                            : 360.0;
    if (dfam > 30.0) CHECK(idx != border_idx);
  }
}

TEST_CASE("same-device second series takes the next free slot") {
  // Phase 6 (Part B): the second series of a panel from the SAME device no
  // longer draws a lighter 50%-white variant. Instead, no two series of a
  // panel may share a color: GTT keeps the device's role color (the iGPU's
  // slot 3, index 2) and VRAM takes the next free theme slot in that panel
  // (slot 1, index 0).
  const ui::Theme theme = ui::Theme::cyberpunk_blue();
  const ftxui::Color gtt_c =
      ui::to_ftxui(theme.series[2], ui::ColorMode::TrueColor);
  const ftxui::Color vram_c =
      ui::to_ftxui(theme.series[0], ui::ColorMode::TrueColor);
  CHECK(gtt_c != vram_c);

  // The iGPU VRAM panel build_default_page emits for Cezanne (ci = 2): GTT
  // is the primary (first) series in the device color; VRAM is second and
  // takes the next free slot.
  {
    const UiFixture fx;  // real demo data: the iGPU reports GTT
    const ui::PanelSpec* vram = nullptr;
    for (const auto& p : fx.page)
      if (p.id == "gpu.0000:07:00.0.vram") vram = &p;
    REQUIRE(vram != nullptr);
    REQUIRE_EQ(vram->series.size(), static_cast<std::size_t>(2));
    CHECK(vram->series[0].id == SeriesId("gpu.0000:07:00.0.gtt"));
    CHECK_EQ(vram->series[0].color_index, 2);
    CHECK(vram->series[1].id == SeriesId("gpu.0000:07:00.0.vram"));
    CHECK_EQ(vram->series[1].color_index, 0);  // next free slot (slot 1)
    // No two series of the panel share a color.
    CHECK(vram->series[0].color_index != vram->series[1].color_index);
  }

  // Rendered: the two legend bars ("▌") are the two distinct series colors,
  // in panel order (GTT then VRAM).
  Collector c;
  c.add(std::make_unique<DemoSource>());
  c.probe_all(/*warm_up=*/true);
  for (int i = 0; i < 70; ++i) c.sample();
  ui::Hardware hw;
  hw.cpu_name = "Test CPU";
  ui::PanelSpec p;
  p.id = "vram";
  p.title = "T \xC2\xB7 Memory";
  p.series.push_back({.id = SeriesId("gpu.0000:07:00.0.gtt"), .label = "GTT",
                      .color_index = 2,
                      .value_fn = [](const Snapshot&) { return std::string("128 MiB"); }});
  p.series.push_back({.id = SeriesId("gpu.0000:07:00.0.vram"), .label = "VRAM",
                      .color_index = 0,
                      .value_fn = [](const Snapshot&) { return std::string("1 GiB"); }});
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
  CHECK_EQ(screen.PixelAt(bars[0], 9).foreground_color, gtt_c);
  CHECK_EQ(screen.PixelAt(bars[1], 9).foreground_color, vram_c);
}

TEST_CASE("0->max->0 spike: unbroken one-dot line on both slopes (two series)") {
  // Multi-series panel -> one-dot braille (no fill). The TOP line (the CPU
  // spike, series 1) must stay continuous: consecutive samples are connected
  // in dot space (Bresenham), so a steep rise/fall is an unbroken vertical
  // run of dots, never isolated dots with gaps. The flat RAM line (series 0,
  // the primary) pins the bottom dot row across the whole window, so the
  // TOPMOST run is always the spike line and bottom_dot is always the bottom.
  ftxui::Screen screen(138, 12);
  render_spike(screen);
  const int gx = 8, gy = 2, Wg = 129, Hg = 7;
  const int dot_cols = 2 * Wg, dot_rows = 4 * Hg;
  const int bottom = dot_rows - 1;  // 27

  // Before the spike (~column 248) both lines sit at 0: a single dot at the
  // bottom row (the two series coincide there).
  for (int xc = 0; xc <= 246; ++xc) {
    CHECK_EQ(top_dot(screen, gx, gy, xc, Hg), bottom);
    CHECK_EQ(bottom_dot(screen, gx, gy, xc, Hg), bottom);
  }
  // The spike region: the topmost (spike) run is contiguous, and adjacent
  // columns' runs touch (no vertical gap) — both slopes.
  int deep_cols = 0;
  for (int xc = 247; xc < dot_cols; ++xc) {
    const int t = top_dot(screen, gx, gy, xc, Hg);
    const int b = top_run_bottom(screen, gx, gy, xc, Hg);
    REQUIRE(t >= 0);  // data all the way to the window end
    CHECK(t <= b);
    // The flat line keeps the bottom dot set in every column.
    CHECK_EQ(bottom_dot(screen, gx, gy, xc, Hg), bottom);
    if (b - t >= 2) ++deep_cols;  // a real vertical run (not a single dot)
    if (xc > 247) {
      const int pt = top_dot(screen, gx, gy, xc - 1, Hg);
      const int pb = top_run_bottom(screen, gx, gy, xc - 1, Hg);
      CHECK(t <= pb + 1);  // the runs overlap or are vertically adjacent
      CHECK(pt <= b + 1);
    }
  }
  // Both slopes are steep enough to contain multi-dot vertical runs.
  CHECK(deep_cols >= 4);
  // The peak is in the middle of the spike (the 100 sample maps to ~column
  // 252); on the 128-unit scale 100 lands at dot row 6, near the top.
  int best = -1, best_xc = -1;
  for (int xc = 248; xc < dot_cols; ++xc)
    if (best < 0 || top_dot(screen, gx, gy, xc, Hg) < best) {
      best = top_dot(screen, gx, gy, xc, Hg);
      best_xc = xc;
    }
  CHECK(best >= 0);
  CHECK(best <= 8);  // the spike nearly reaches the top
  CHECK(best_xc >= 250);
  CHECK(best_xc <= 256);
  // The window ends back at 0 (the last column's topmost dot is the flat line
  // / spike baseline at the bottom).
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

// ---------------------------------------------------------------------------
// v1.0.2: the scroll indicator and the resize sequence

// The footer line (the last line of the frame) as plain text.
std::string footer_text(const ftxui::Screen& screen, int H) {
  const std::string text = ui::screen_to_text(screen);
  std::vector<std::string> lines;
  std::size_t start = 0;
  for (std::size_t i = 0; i <= text.size(); ++i)
    if (i == text.size() || text[i] == '\n') {
      lines.push_back(text.substr(start, i - start));
      start = i + 1;
    }
  return lines.size() >= (std::size_t)H ? lines[H - 1] : "";
}

// The v1.0.2 invariant in its render-level form: the VISIBLE panels tile the
// content area exactly — no overlaps, and either the panel rects' areas sum
// to the whole region (no scrolling) or each visible row tiles the full width
// and the k rows sum to the content height (scrolling). The full version is
// the sweep in test_layout.cpp.
void check_tiles(const ui::Layout& lo, int W, int H) {
  const int ch = H - 2;
  CHECK_EQ(lo.content_height, ch);
  int sum = 0;
  for (int rh : lo.row_heights) sum += rh;
  CHECK_EQ(sum, ch);  // the visible rows fill the viewport exactly
  for (int i = 0; i < (int)lo.panels.size(); ++i) {
    const auto& a = lo.panels[i].rect;
    CHECK(a.x >= 0);
    CHECK(a.x + a.w <= W);
    CHECK(a.y >= 0);
    for (int j = i + 1; j < (int)lo.panels.size(); ++j) {
      const auto& b = lo.panels[j].rect;
      const bool overlap = a.x < b.x + b.w && b.x < a.x + a.w &&
                           a.y < b.y + b.h && b.y < a.y + a.h;
      CHECK_FALSE(overlap);  // no overlap
    }
  }
  if (!lo.scrolling) {
    // Contained + non-overlapping + areas sum to the region's area
    //  =>  the union is exactly the region: no gaps, no blank rows.
    long long area = 0;
    for (const auto& pl : lo.panels)
      area += static_cast<long long>(pl.rect.w) * pl.rect.h;
    CHECK_EQ(area, static_cast<long long>(W) * ch);
  } else {
    CHECK(lo.total_rows > lo.rows());
    for (int r = 0; r < lo.rows(); ++r) {  // the stored (offset-0) visible rows
      int rowmin = 1 << 30, rowmax = 0, rowsum = 0;
      bool any = false;
      for (const auto& pl : lo.panels) {
        const auto& rc = pl.rect;
        if (rc.row != r) continue;
        any = true;
        rowmin = std::min(rowmin, rc.x);
        rowmax = std::max(rowmax, rc.x + rc.w);
        rowsum += rc.w;
      }
      CHECK(any);
      CHECK_EQ(rowmin, 0);
      CHECK_EQ(rowmax, W);
      CHECK_EQ(rowsum, W);  // + non-overlap => no horizontal gaps
      CHECK(lo.row_heights[r] >= ui::tight_height());
    }
  }
}

TEST_CASE("scroll indicator: 'N more' with conditional arrows (10-panel page)") {
  UiFixture fx;
  // 60x20: content 18 rows -> k = 4 visible Tight rows of a 10-row grid,
  // 6 rows hidden, offset 0..6.
  auto foot_at = [&](int top_row) {
    ui::FrameState st;
    ui::LayoutCache cache = cache_for(fx.page);
    ftxui::Screen screen(60, 20);
    screen.Clear();
    ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr, st,
                     ui::Theme::cyberpunk_blue(), ui::ColorMode::TrueColor,
                     /*paused=*/false, 1000, cache, top_row, kWinNs);
    return footer_text(screen, 20);
  };
  const std::string kUp = "\xE2\x96\xB2";    // "▲"
  const std::string kDown = "\xE2\x96\xBC";  // "▼"
  const std::string kUpDown = "\xE2\x86\x91\xE2\x86\x93";  // "↑↓"
  const std::string top = foot_at(0);
  CHECK(top.find("6 more " + kDown) != std::string::npos);
  CHECK(top.find(kUp) == std::string::npos);  // nothing hidden above
  const std::string mid = foot_at(3);
  CHECK(mid.find(kUp + " 6 more " + kDown) != std::string::npos);
  const std::string bottom = foot_at(6);
  CHECK(bottom.find(kUp + " 6 more") != std::string::npos);
  CHECK(bottom.find(kDown) == std::string::npos);  // nothing hidden below
  // The "↑↓ scroll" hint is present at every position.
  for (const auto& f : {top, mid, bottom})
    CHECK(f.find(kUpDown + " scroll") != std::string::npos);
}

TEST_CASE("scroll indicator survives right-truncation at narrow widths") {
  UiFixture fx;
  // 44x20: one column, 10 rows -> scrolls (6 hidden). The full footer (hints
  // + scroll tail) is far wider than 44, so the KEY HINTS are the part that
  // gets cut — the scroll tail must still be fully visible, right-aligned.
  ui::FrameState st;
  ui::LayoutCache cache = cache_for(fx.page);
  ftxui::Screen screen(44, 20);
  screen.Clear();
  ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr, st,
                   ui::Theme::cyberpunk_blue(), ui::ColorMode::TrueColor,
                   /*paused=*/false, 1000, cache, 0, kWinNs);
  const std::string foot = footer_text(screen, 20);
  CHECK(foot.find("\xE2\x86\x91\xE2\x86\x93 scroll") != std::string::npos);
  CHECK(foot.find("6 more") != std::string::npos);
  CHECK(foot.find("\xE2\x96\xBC") != std::string::npos);  // at top: "6 more ▼"
  // The frame must still be exactly 44x20: hints truncated, tail intact.
  const ui::GeometryReport rep =
      ui::check_geometry(ui::screen_to_text(screen), 44);
  CHECK_EQ(rep.lines, 20);
  CHECK(rep.bad_lines.empty());
}

TEST_CASE("resize sequence: the layout retiles at every step (in-process)") {
  // The work-order resize sequence, driven through compute_layout + the frame
  // renderer at each size (the in-process form of the SIGWINCH test).
  UiFixture fx;
  const ui::Theme theme = ui::Theme::cyberpunk_blue();
  const std::vector<std::pair<int, int>> seq = {
      {300, 80}, {210, 20}, {120, 40}, {80, 24}, {160, 48}};
  for (const auto& [W, H] : seq) {
    const ui::Layout lo = ui::compute_layout(W, H, fx.page, ui::LayoutPrefs{});
    check_tiles(lo, W, H);
    ui::FrameState st;
    ui::LayoutCache cache = cache_for(fx.page);
    ftxui::Screen screen(W, H);
    screen.Clear();
    ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr,
                     st, theme, ui::ColorMode::TrueColor, /*paused=*/false, 1000,
                     cache, 0, kWinNs);
    const ui::GeometryReport rep =
        ui::check_geometry(ui::screen_to_text(screen), W);
    CHECK_EQ(rep.lines, H);
    CHECK(rep.bad_lines.empty());
    if (lo.scrolling)  // the indicator is present whenever scrolling
      CHECK(footer_text(screen, H).find("more") != std::string::npos);
  }
  // 160x48 is the standing non-regression size: all Full, no scrolling.
  {
    const ui::Layout lo =
        ui::compute_layout(160, 48, fx.page, ui::LayoutPrefs{});
    CHECK_FALSE(lo.scrolling);
    for (const auto& pl : lo.panels) CHECK_EQ(pl.mode, ui::PanelMode::kFull);
  }
}

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// v1.0.3 Part A: the RENDERED Power value must follow a changing PPT, with
// the iGPU panels visible, hidden, and toggled mid-run. Real sources
// (CpuPowerSource + GpuRegistrySource) over a fake /sys whose power1_input
// is rewritten between ticks — the pre-fix freeze showed up here first, in
// the frame, not in the snapshot.

namespace {

struct ApuRenderFixture {
  test::FakePciTree tree;
  Collector c;
  ui::Hardware hw;
  std::vector<ui::PanelSpec> page;
  std::string bus = "0000:07:00.0";

  ApuRenderFixture() {
    tree.add_device(bus, "1002", "1638", "amdgpu", true, "active", 0);
    tree.write_dev_file(bus, "hwmon/hwmon0/name", "amdgpu\n");
    tree.write_dev_file(bus, "hwmon/hwmon0/power1_input", "20000000\n");
    tree.write_dev_file(bus, "gpu_busy_percent", "3\n");
    tree.write_dev_file(bus, "mem_info_vram_total", "536870912\n");  // 512 MiB
    tree.write_dev_file(bus, "mem_info_vram_used", "104857600\n");

    c.add(std::make_unique<CpuPowerSource>(tree.root.string()));
    auto reg = std::make_unique<GpuRegistrySource>();
    reg->set_sysfs_roots(tree.root.string(), (tree.root / "class/drm").string());
    reg->set_pci_ids_path(std::string(GETH_TEST_FIXTURES) + "/pci_ids.txt");
    c.add(std::move(reg));
    c.probe_all(/*warm_up=*/true);
    for (int i = 0; i < 10; ++i) {  // history to fill the Power graph
      tree.write_dev_file(bus, "hwmon/hwmon0/power1_input",
                          std::to_string(20000000 + i * 1000000) + "\n");
      c.sample();
    }
    hw = ui::hardware_from(c.latest(), "Fake APU");
    page = ui::build_default_page(hw);
  }
};

}  // namespace

TEST_SUITE("apu_power_render") {
  // The 160x48 frame as plain text (no SGR codes).
  std::string render_frame_text(ApuRenderFixture& fx) {
    ui::FrameState state;
    ui::LayoutCache cache = cache_for(fx.page);
    ftxui::Screen screen(160, 48);
    screen.Clear();
    ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr,
                     state, ui::Theme::cyberpunk_blue(), ui::ColorMode::TrueColor,
                     /*paused=*/false, 1000, cache, 0, kWinNs);
    return ui::screen_to_text(screen);
  }

  // Rewrite the fake sensor and take one tick, as the app would.
  void set_watts(ApuRenderFixture& fx, double w) {
    fx.tree.write_dev_file(fx.bus, "hwmon/hwmon0/power1_input",
                           std::to_string(static_cast<unsigned long long>(w * 1e6)) +
                               "\n");
    fx.c.sample();
  }

  TEST_CASE("rendered Power follows a changing PPT (visible / hidden / toggled)") {
    ApuRenderFixture fx;
    int gpu_panels = 0, power = 0;
    for (const auto& p : fx.page) {
      if (p.id.rfind("gpu.", 0) == 0) ++gpu_panels;
      if (p.id == "power") ++power;
    }
    REQUIRE_EQ(gpu_panels, 2);
    REQUIRE_EQ(power, 1);

    // iGPU panels visible: the rendered watts follow the sensor.
    set_watts(fx, 30.0);
    CHECK(render_frame_text(fx).find("30.0 W") != std::string::npos);
    set_watts(fx, 40.0);
    CHECK(render_frame_text(fx).find("40.0 W") != std::string::npos);

    // Hide both iGPU panels (the user's 5600H configuration).
    for (auto& p : fx.page)
      if (p.id == "gpu.0000:07:00.0.usage" || p.id == "gpu.0000:07:00.0.vram")
        p.visible = false;
    set_watts(fx, 50.0);
    CHECK(render_frame_text(fx).find("50.0 W") != std::string::npos);

    // Toggle one back mid-run: still follows.
    for (auto& p : fx.page)
      if (p.id == "gpu.0000:07:00.0.usage") p.visible = true;
    set_watts(fx, 35.0);
    CHECK(render_frame_text(fx).find("35.0 W") != std::string::npos);
  }
}
