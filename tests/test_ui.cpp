// UI tests: the transparency guarantee (GETH never paints a background color)
// and the "never crash at any terminal size" requirement. These drive the real
// render_frame() through an offscreen ftxui::Screen and inspect the SGR output.

#include <doctest.h>

#include <cctype>
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

}  // TEST_SUITE
