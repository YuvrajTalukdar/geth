// UI tests: the transparency guarantee (GETH never paints a background color)
// and the "never crash at any terminal size" requirement. These drive the real
// render_frame() through an offscreen ftxui::Screen and inspect the SGR output.

#include <doctest.h>

#include <algorithm>
#include <cctype>
#include <chrono>
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

using namespace geth;

namespace {

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

// True if any SGR sequence in `s` paints a background or inverts text — the
// machine-checked form of the "no background" rule (48;2;…, 48;5;…, 4X, 10X,
// or reverse 7). An extended color (38;… / 48;…) consumes its mode + argument
// params so a *foreground* color component (e.g. the "47" in "38;2;20;89;47")
// is not mistaken for a standalone palette-16 background code. (SGR 4 alone is
// underline, not a background, and is allowed.)
bool has_forbidden_style(const std::string& s) {
  for (std::size_t i = 0; i + 2 < s.size(); ++i) {
    if (s[i] != 0x1b || s[i + 1] != '[') continue;
    std::size_t j = i + 2;
    std::size_t start = j;
    while (j < s.size() &&
           (std::isdigit(static_cast<unsigned char>(s[j])) || s[j] == ';'))
      ++j;
    if (j >= s.size() || s[j] != 'm') {
      i = j;
      continue;
    }
    std::vector<int> pv;
    {
      std::string cur;
      for (std::size_t k = start; k < j; ++k) {
        if (s[k] == ';') {
          if (!cur.empty()) pv.push_back(std::atoi(cur.c_str()));
          cur.clear();
        } else {
          cur += s[k];
        }
      }
      if (!cur.empty()) pv.push_back(std::atoi(cur.c_str()));
    }
    std::size_t p = 0;
    while (p < pv.size()) {
      const int v = pv[p++];
      if (v == 38 || v == 48) {
        if (v == 48) return true;  // an extended background
        if (p < pv.size()) {
          const int mode = pv[p++];
          p += (mode == 2) ? 3 : 1;  // 2;r;g;b or 5;n
        }
      } else if (v == 7 || (v >= 40 && v <= 47) || (v >= 100 && v <= 107)) {
        return true;  // reverse, or a palette-16 background
      }
    }
    i = j;
  }
  return false;
}

void render_at(int W, int H, const UiFixture& fx, const ui::Theme& theme,
               ui::ColorMode mode) {
  ui::FrameState state;
  ftxui::Screen screen(W, H);
  screen.Clear();
  ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr, state,
                   theme, mode, /*paused=*/false, 1000);
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

// Renders a single CPU panel on a 138x12 screen: the panel fills (0,1) at
// 69x10, its graph area is exactly 60 braille columns x 7 rows (120 x 28
// dots), with gx = 8, gy = 2, and "100 %" as the widest tick label (L = 5:
// Wg = (69-2) - 5 - 2 = 60).
void render_ramp(bool gap, ftxui::Screen& screen) {
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
  sr.color_index = 0;
  sr.fill = true;
  p.series.push_back(sr);
  std::vector<ui::PanelSpec> page{p};
  ui::FrameState state;
  ui::render_frame(screen, hw, page, c.store(), c.latest(), nullptr, state,
                   ui::Theme::cyberpunk_blue(), ui::ColorMode::TrueColor,
                   /*paused=*/false, 1000);
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

}  // namespace

TEST_SUITE("ui") {

TEST_CASE("no background color is ever emitted (160x48, all color modes)") {
  const UiFixture fx;
  const ui::Theme theme = ui::Theme::cyberpunk_blue();
  const ui::ColorMode modes[] = {ui::ColorMode::TrueColor, ui::ColorMode::Color256,
                                 ui::ColorMode::Color16};
  for (auto mode : modes) {
    ui::FrameState state;
    ftxui::Screen screen(160, 48);
    screen.Clear();
    ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr, state,
                     theme, mode, /*paused=*/false, 1000);
    const std::string out = screen.ToString();
    // Sanity: a frame was actually drawn (borders, title, footer).
    CHECK(!out.empty());
    CHECK(out.find('\xe2') != std::string::npos);  // some UTF-8 (box/braille) glyph
    CHECK(!has_forbidden_style(out));
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
    ftxui::Screen screen(39, 11);
    screen.Clear();
    ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr, state,
                     theme, ui::ColorMode::TrueColor, false, 1000);
    const std::string out = ui::screen_to_text(screen);
    CHECK(out.find("terminal too small") != std::string::npos);
  }
  {
    ui::FrameState state;
    ftxui::Screen screen(40, 12);
    screen.Clear();
    ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr, state,
                     theme, ui::ColorMode::TrueColor, false, 1000);
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
    ftxui::Screen screen(sz[0], sz[1]);
    screen.Clear();
    ui::render_frame(screen, fx.hw, fx.page, fx.c.store(), fx.c.latest(), nullptr,
                     state, theme, ui::ColorMode::TrueColor, false, 1000);
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
  const int gx = 8, gy = 2, Wg = 60, Hg = 7;
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
  // Gap el 25..35 s of the 59 s span -> dot columns ~49..69.
  for (int xc = 50; xc <= 68; ++xc) CHECK_EQ(top_dot(screen, gx, gy, xc, Hg), -1);
  CHECK(top_dot(screen, gx, gy, 45, Hg) >= 0);  // data before the gap
  CHECK(top_dot(screen, gx, gy, 72, Hg) >= 0);  // data after the gap
}

TEST_CASE("filled graph: line in series color, fill below in the derived color") {
  ftxui::Screen screen(138, 12);
  render_ramp(/*gap=*/false, screen);
  const int gx = 8, gy = 2, Wg = 60, Hg = 7;
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

}  // TEST_SUITE
