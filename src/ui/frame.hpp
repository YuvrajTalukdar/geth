// One full UI frame: header line, the 2-column panel grid, footer line.
//
// The frame is drawn by writing braille glyphs, box glyphs and text directly
// into ftxui::Screen pixels from a custom ftxui::Node. (FTXUI's Canvas is a
// 1-character-per-pixel surface; a braille cell carries 4 dots and the
// fill+line rendering needs two colors in one cell, so the Canvas API cannot
// express it — the Screen is the same FTXUI pixel model the Canvas would
// composite into, addressed directly.)
//
// Transparency guarantee: by default the renderer only ever sets
// `foreground_color` and style bits; `background_color` stays Color::Default
// (Palette1, which FTXUI prints as SGR 49, never 48). The no-background unit
// test asserts this. The ONE exception: with `FrameChrome::solid_bg` (config
// background = "solid") a final pass paints the theme's bg color across every
// pixel — the only code path in GETH allowed to emit a background color.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/screen.hpp"

#include "ui/layout.hpp"
#include "ui/panel_spec.hpp"
#include "ui/theme.hpp"

namespace geth::ui {

// -- Braille dot encoding (exposed for the unit tests) ------------------------
// The braille bit of dot (col 0/1, row 0..3 top->bottom) in one 2x4 cell:
// col 0 -> bits {0,1,2,6}, col 1 -> bits {3,4,5,7}, so all eight dots set the
// mask 0xFF and the full cell is U+28FF (⣿).
int braille_bit(int col, int row);

// U+2800|mask as UTF-8.
std::string braille_char(std::uint8_t mask);

// -- Y-axis scales (exposed for the unit tests) --------------------------------
// Round up to a nice power-of-two multiple (1/2/4/8/16/32/64/128 x 1024^n):
// 15 MiB -> 16 MiB, 100 MiB -> 128 MiB (binary units are powers of two).
double nice_bytes(double v);
// Absolute scales (RAM/VRAM totals): the smallest value >= v from the
// sequence {1, 1.5} x 2^k, so the top tick lands on a round number in the
// display unit — 22.8 GiB -> 24 GiB, 15.4 GiB -> 16 GiB, 4 GiB -> 4 GiB,
// 512 MiB -> 512 MiB. (1024^n is itself a power of two, so computing the
// ceiling on raw bytes gives the same value as rounding in the display unit.)
double nice_bytes_max(double v);
// Round up to a nice number: 1 / 2 / 2.5 / 5 x 10^n.
double nice_watts(double v);

// -- Geometry self-check -------------------------------------------------------
// Line numbers (1-based) of lines whose DISPLAY width (ftxui::string_width,
// i.e. wcwidth — columns, not bytes or code points) is not `want_cols`. A
// single trailing newline does not start an extra line.
struct GeometryReport {
  int lines = 0;
  std::vector<int> bad_lines;
  bool ok(int want_lines) const {
    return lines == want_lines && bad_lines.empty();
  }
};
GeometryReport check_geometry(const std::string& text, int want_cols);

// Startup measurement: main() calls frame_mark_process_start() before
// anything else; the first note_render() (below) measures against it.
void frame_mark_process_start();
// The marked start point in steady-clock ns (0 = not marked).
long long process_start_ns();

// --stats bookkeeping (steady_clock, printed on exit).
struct FrameStats {
  unsigned long long frames = 0, samples = 0;
  double render_us_total = 0, render_us_max = 0;
  double sample_us_total = 0, sample_us_max = 0;
  long long allocs_total = 0, allocs_max = 0;
  // Startup time: process start (frame_mark_process_start) to the first full
  // draw, in ms; -1 when unmeasured.
  double first_frame_ms = -1;

  void note_render(double us, long long allocs) {
    if (frames == 0) {
      const long long s0 = process_start_ns();
      if (s0 > 0)
        first_frame_ms = std::chrono::duration<double, std::milli>(
            Clock::now().time_since_epoch() -
            std::chrono::nanoseconds(s0)).count();
    }
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

// The y-axis max for this frame, updating the sticky/hysteresis state.
// (Declared here, after FrameState; the definition is in frame.cpp.)
double panel_y_max(const PanelSpec& p, double visible_max, FrameState& st,
                   TimePoint now);

// The computed Layout, cached by terminal size. Recomputes ONLY when (W, H)
// changes — i.e. on a resize — and reuses the stored vectors otherwise, so a
// frame at an unchanged size allocates nothing for the layout (spec). The app
// loop owns one and points it at the page; render_frame pulls from it.
//
// Phase 4: the layout is computed over the VISIBLE panels only; `visible_idx`
// maps layout/panel index -> index into *page. Call invalidate() after any
// page change (a panel hidden/shown/renamed) so the next get() rebuilds.
struct LayoutCache {
  const std::vector<PanelSpec>* page = nullptr;
  LayoutPrefs prefs;
  Layout layout;
  std::vector<int> visible_idx;
  int w = -1, h = -1;
  void invalidate() { w = -1; }
  const Layout& get(int W, int H) {
    if (page && (W != w || H != h)) {
      std::vector<PanelSpec> vis;
      visible_idx.clear();
      vis.reserve(page->size());
      for (std::size_t i = 0; i < page->size(); ++i)
        if ((*page)[i].visible) {
          visible_idx.push_back((int)i);
          vis.push_back((*page)[i]);
        }
      layout = compute_layout(W, H, vis, prefs);
      w = W;
      h = H;
    }
    return layout;
  }
};

// App-level frame extras (Phase 4). All optional: empty / -1 / false gives
// exactly the plain dashboard of Phases 1-3.
struct FrameChrome {
  std::string config_warning;  // "line N: msg" — the persistent footer warn
  std::string toast;           // e.g. "saved"; shown while now < toast_until
  TimePoint toast_until{};
  // Phase 5: [general].title ("" = the auto header title).
  std::string title_override;
  // Phase 5: background = "solid" — paint the theme's bg across the whole
  // screen (the single sanctioned background-color path).
  bool solid_bg = false;
};

// The panel-manager overlay (drawn over the live dashboard, which keeps
// relaying out behind it — the live preview).
struct ManagerOverlay {
  struct Row {
    std::string label;
    bool visible = true;
    bool present = true;  // false: absent hardware, dimmed "(not present)"
    bool selected = false;
  };
  std::vector<Row> rows;
  int scroll = 0;  // first row index shown
  // `r`: inline rename of rename_row (the edit buffer, with a block cursor).
  bool renaming = false;
  int rename_row = -1;
  std::string rename_buf;
  // Enter: the series submenu of the selected panel.
  bool submenu = false;
  std::string submenu_title;  // "Series — CPU"
  struct SubRow {
    std::string label;
    bool hidden = false;
    bool selected = false;
  };
  std::vector<SubRow> sub_rows;
  // `d`: pending "reset panels to defaults? (y/n)".
  bool confirm_reset = false;
};

// The `?` help overlay.
struct HelpOverlay {
  std::string title = "Keys";
  std::vector<std::pair<std::string, std::string>> lines;  // (keys, meaning)
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
                           const SeriesStore& store, const Snapshot& latest,
                           int64_t window_ns);

// Draws one complete frame into `screen` (which must already be sized
// W x H — use screen.dimx()/dimy()). `frozen` (when paused) overrides the
// live store/snapshot. `state` is updated (scale hysteresis). Panels are laid
// out by `cache` (recomputed only on a size change) and drawn from `top_row`
// (the scroll position, a row index); graphs cover `window_ns`. Never writes
// outside the screen; below 40x12 it draws the "terminal too small" notice.
//
// Phase 4: `chrome` carries the footer warning/toast, `focus_panel` (a PAGE
// index, -1 = none) gets the accent border, and `manager` / `help` are the
// overlays drawn last (the manager over the live dashboard, the help over
// everything).
void render_frame(ftxui::Screen& screen, const Hardware& hw,
                  const std::vector<PanelSpec>& page, const SeriesStore& store,
                  const Snapshot& latest, const FrozenFrame* frozen,
                  FrameState& state, const Theme& theme, ColorMode mode,
                  bool paused, long long interval_ms, LayoutCache& cache,
                  int top_row, int64_t window_ns, const FrameChrome& chrome = {},
                  int focus_panel = -1, const ManagerOverlay* manager = nullptr,
                  const HelpOverlay* help = nullptr);

// The Element form of the above, for the ScreenInteractive loop.
ftxui::Element build_frame_element(const Hardware& hw,
                                   const std::vector<PanelSpec>& page,
                                   const SeriesStore& store, const Snapshot& latest,
                                   const FrozenFrame* frozen, FrameState& state,
                                   const Theme& theme, ColorMode mode, bool paused,
                                   long long interval_ms, LayoutCache& cache,
                                   int top_row, int64_t window_ns,
                                   const FrameChrome& chrome = {},
                                   int focus_panel = -1,
                                   const ManagerOverlay* manager = nullptr,
                                   const HelpOverlay* help = nullptr);

// Plain UTF-8 text of a rendered screen (for --render-frame without --ansi).
std::string screen_to_text(const ftxui::Screen& screen);

}  // namespace geth::ui
