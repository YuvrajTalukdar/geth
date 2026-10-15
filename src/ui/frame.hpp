// One full UI frame: header line, the 2-column panel grid, footer line.
//
// The frame is drawn by writing braille glyphs, box glyphs and text directly
// into ftxui::Screen pixels from a custom ftxui::Node. (FTXUI's Canvas is a
// 1-character-per-pixel surface; a braille cell carries 4 dots and the
// fill+line rendering needs two colors in one cell, so the Canvas API cannot
// express it — the Screen is the same FTXUI pixel model the Canvas would
// composite into, addressed directly.)
//
// Transparency guarantee: the renderer only ever sets `foreground_color` and
// style bits; `background_color` stays Color::Default (Palette1, which FTXUI
// prints as SGR 49, never 48). The no-background unit test asserts this.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"

#include "ui/panel_spec.hpp"
#include "ui/theme.hpp"

namespace geth::ui {

// --stats bookkeeping (steady_clock, printed on exit).
struct FrameStats {
  unsigned long long frames = 0, samples = 0;
  double render_us_total = 0, render_us_max = 0;
  double sample_us_total = 0, sample_us_max = 0;
  long long allocs_total = 0, allocs_max = 0;

  void note_render(double us, long long allocs) {
    ++frames;
    render_us_total += us;
    render_us_max = std::max(render_us_max, us);
    allocs_total += allocs;
    allocs_max = std::max(allocs_max, allocs);
  }
  void note_sample(double us) {
    ++samples;
    sample_us_total += us;
    sample_us_max = std::max(sample_us_max, us);
  }
  double render_us_mean() const { return frames ? render_us_total / frames : 0; }
  double sample_us_mean() const { return samples ? sample_us_total / samples : 0; }
  double allocs_mean() const { return frames ? static_cast<double>(allocs_total) / frames : 0; }
  void print() const;
};

// Per-panel scale state that must survive across frames: the auto-scale
// hysteresis ("shrink only after 10 s below 50 %") and the sticky
// temperature ceiling (100 -> 110 -> 120).
struct FrameState {
  double temp_max = 100.0;
  struct Auto {
    double max = 0;
    TimePoint below_since{};
    bool below = false;
  };
  std::map<std::string, Auto> auto_scale;  // keyed by panel id
};

// A frozen copy of the display window, taken on pause so the display freezes
// while sampling continues.
struct FrozenFrame {
  Snapshot snap;
  struct Window {
    SeriesId id;
    std::vector<int64_t> t;  // in-window samples, oldest -> newest
    std::vector<float> v;    // NaN = gap
    int64_t t_start = 0, t_end = 0;
    double span_ns = 0;
  };
  std::vector<Window> windows;
  const Window* find(const SeriesId& id) const {
    for (const auto& w : windows)
      if (w.id == id) return &w;
    return nullptr;
  }
};

FrozenFrame capture_frozen(const std::vector<PanelSpec>& page,
                           const SeriesStore& store, const Snapshot& latest);

// Draws one complete frame into `screen` (which must already be sized
// W x H — use screen.dimx()/dimy()). `frozen` (when paused) overrides the
// live store/snapshot. `state` is updated (scale hysteresis). Never writes
// outside the screen; below 40x12 it draws the "terminal too small" notice.
void render_frame(ftxui::Screen& screen, const Hardware& hw,
                  const std::vector<PanelSpec>& page, const SeriesStore& store,
                  const Snapshot& latest, const FrozenFrame* frozen,
                  FrameState& state, const Theme& theme, ColorMode mode,
                  bool paused, long long interval_ms);

// The Element form of the above, for the ScreenInteractive loop.
ftxui::Element build_frame_element(const Hardware& hw,
                                   const std::vector<PanelSpec>& page,
                                   const SeriesStore& store, const Snapshot& latest,
                                   const FrozenFrame* frozen, FrameState& state,
                                   const Theme& theme, ColorMode mode, bool paused,
                                   long long interval_ms);

// Plain UTF-8 text of a rendered screen (for --render-frame without --ansi).
std::string screen_to_text(const ftxui::Screen& screen);

}  // namespace geth::ui
