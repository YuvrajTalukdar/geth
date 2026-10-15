#include "ui/app.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <optional>

#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"

#include "util/alloc_count.hpp"

namespace geth::ui {

bool resolve_color_mode(const UiOptions& opts, ColorMode& out) {
  if (opts.color_mode.empty()) {
    out = detect_color_mode();
    return true;
  }
  if (!parse_color_mode(opts.color_mode, out)) {
    std::fprintf(stderr, "geth: --color must be truecolor, 256 or 16\n");
    return false;
  }
  return true;
}

namespace {

// The graph time window, cycled by `t`: 30 s -> 1 m -> 2 m -> 5 m -> 10 m ->
// 30 s. The default (index 1) is 1 m, the pre-Phase-3 fixed 60 s window.
constexpr int64_t kWindowsNs[] = {
    30LL * 1000000000LL,    // 30 s
    60LL * 1000000000LL,    // 1 m
    120LL * 1000000000LL,   // 2 m
    300LL * 1000000000LL,   // 5 m
    600LL * 1000000000LL,   // 10 m
};
constexpr int kNumWindows = 5;
constexpr int kDefaultWindow = 1;  // 1 m

// The whole dashboard as one FTXUI component: the event handler does the
// sampling, OnRender builds the frame. FTXUI's Draw() calls OnRender() after
// every event that invalidates the frame (tick, resize, key press), so the
// redraw-on-tick/resize/key spec falls out of the loop itself — no timer in
// the render path, no fixed-FPS animation.
class GethComponent : public ftxui::ComponentBase {
 public:
  GethComponent(Collector& collector, const Hardware& hw,
                const std::vector<PanelSpec>& page, const Theme& theme,
                ColorMode mode, long long interval_ms, unsigned long long count,
                int columns, FrameStats& stats)
      : _collector(collector),
        _hw(hw),
        _page(page),
        _theme(theme),
        _mode(mode),
        _interval_ms(interval_ms),
        _count(count),
        _stats(stats) {
    _cache.page = &page;
    if (columns > 0) _cache.prefs.columns = columns;
  }

  bool OnEvent(ftxui::Event event) override {
    // Every event (resizes above all) goes through the layout cache first:
    // get() recomputes ONLY when the terminal size changed since the last
    // call and reuses the stored layout otherwise. While scrolling, keep
    // mouse tracking in step with the overflow state (FTXUI's default is
    // ON; the spec asks for the wheel to be tracked only while the page
    // overflows) and clamp the scroll position into the new range.
    if (event.screen_) {
      const int W = event.screen_->dimx(), H = event.screen_->dimy();
      const Layout& lo = _cache.get(W, H);
      if (lo.scrolling != _mouse_tracked) {
        event.screen_->TrackMouse(lo.scrolling);
        _mouse_tracked = lo.scrolling;
      }
      _top_row = std::clamp(_top_row, 0, max_top_row(lo, H - 2));
    }

    // Tick from the timer thread: sampling ALWAYS continues (even while
    // paused — the spec: "Sampling continues; the display freezes").
    if (event == ftxui::Event::Custom) {
      const auto t0 = Clock::now();
      _collector.sample();
      _stats.note_sample(
          std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
      if (++_ticks >= _count && _count != 0 && event.screen_)
        event.screen_->Exit();
      return true;
    }
    if (event == ftxui::Event::Character('q') ||
        event == ftxui::Event::Character('Q') || event == ftxui::Event::Escape ||
        event == ftxui::Event::CtrlC) {
      // FTXUI restores the terminal (alternate screen, termios, cursor) on
      // Exit, including when the loop ends via a crash signal.
      if (event.screen_) event.screen_->Exit();
      return true;
    }
    if (event == ftxui::Event::Character(' ')) {
      _paused = !_paused;
      if (_paused)
        _frozen = capture_frozen(_page, _collector.store(), _collector.latest(),
                                 kWindowsNs[_window_idx]);
      else
        _frozen.reset();
      return true;
    }
    // t: cycle the graph time window (30 s -> 1 m -> 2 m -> 5 m -> 10 m).
    if (event == ftxui::Event::Character('t')) {
      _window_idx = (_window_idx + 1) % kNumWindows;
      return true;
    }

    // Scrolling (no-ops unless the page overflows).
    auto line_scroll = [&](int dir) {
      if (!event.screen_) return;
      const int W = event.screen_->dimx(), H = event.screen_->dimy();
      const Layout& lo = _cache.get(W, H);
      if (!lo.scrolling) return;
      _top_row = std::clamp(_top_row + dir, 0, max_top_row(lo, H - 2));
    };
    auto page_scroll = [&](int dir) {
      if (!event.screen_) return;
      const int W = event.screen_->dimx(), H = event.screen_->dimy();
      const Layout& lo = _cache.get(W, H);
      if (!lo.scrolling) return;
      _top_row =
          std::clamp(_top_row + dir * rows_fitting(lo, H - 2), 0,
                     max_top_row(lo, H - 2));
    };
    if (event == ftxui::Event::ArrowDown ||
        event == ftxui::Event::Character('j'))
      return (line_scroll(1), true);
    if (event == ftxui::Event::ArrowUp ||
        event == ftxui::Event::Character('k'))
      return (line_scroll(-1), true);
    if (event == ftxui::Event::PageDown) return (page_scroll(1), true);
    if (event == ftxui::Event::PageUp) return (page_scroll(-1), true);
    if (event == ftxui::Event::Home) {
      _top_row = 0;
      return true;
    }
    if (event == ftxui::Event::End) {
      if (event.screen_) {
        const int W = event.screen_->dimx(), H = event.screen_->dimy();
        const Layout& lo = _cache.get(W, H);
        _top_row = max_top_row(lo, H - 2);
      }
      return true;
    }
    // Mouse wheel: one row per notch, only while the page overflows.
    if (event.is_mouse()) {
      const ftxui::Mouse& m = event.mouse();
      if (m.button == ftxui::Mouse::WheelUp && m.motion == ftxui::Mouse::Pressed)
        return (line_scroll(-1), true);
      if (m.button == ftxui::Mouse::WheelDown &&
          m.motion == ftxui::Mouse::Pressed)
        return (line_scroll(1), true);
    }
    return false;
  }

  ftxui::Element OnRender() override {
    const FrozenFrame* frozen = _paused ? &_frozen.value() : nullptr;
    const auto t0 = Clock::now();
    const long long a0 = alloc_count();
    auto el = build_frame_element(_hw, _page, _collector.store(),
                                  _collector.latest(), frozen, _state, _theme,
                                  _mode, _paused, _interval_ms, _cache,
                                  _top_row, kWindowsNs[_window_idx]);
    _stats.note_render(
        std::chrono::duration<double, std::micro>(Clock::now() - t0).count(),
        alloc_count() - a0);
    return el;
  }

 private:
  Collector& _collector;
  const Hardware& _hw;
  const std::vector<PanelSpec>& _page;
  const Theme& _theme;
  ColorMode _mode;
  long long _interval_ms;
  unsigned long long _count;
  FrameStats& _stats;

  LayoutCache _cache;
  int _top_row = 0;
  int _window_idx = kDefaultWindow;
  bool _mouse_tracked = true;  // FTXUI's default: mouse tracking ON

  FrameState _state;
  bool _paused = false;
  std::optional<FrozenFrame> _frozen;
  unsigned long long _ticks = 0;
};

}  // namespace

int run_ui(Collector& collector, const Hardware& hw, const UiOptions& opts) {
  ColorMode mode;
  if (!resolve_color_mode(opts, mode)) return 2;
  const Theme theme = Theme::cyberpunk_blue();
  const std::vector<PanelSpec> page = build_default_page(hw);

  ftxui::ScreenInteractive screen = ftxui::ScreenInteractive::Fullscreen();
  FrameStats stats;
  auto component = ftxui::Make<GethComponent>(
      collector, hw, page, theme, mode, opts.interval_ms, opts.count,
      opts.columns, stats);

  // The small timer thread: posts Event::Custom at each tick and nothing
  // else. All sampling and rendering happens on the UI thread.
  std::atomic<bool> quit{false};
  std::thread timer([&screen, &quit, interval = opts.interval_ms] {
    while (!quit) {
      std::this_thread::sleep_for(std::chrono::milliseconds(interval));
      if (quit) break;
      screen.PostEvent(ftxui::Event::Custom);
    }
  });

  screen.Loop(component);  // blocks until q / Esc / Ctrl-C (or count ticks)

  quit = true;
  timer.join();

  if (opts.stats) stats.print();
  return 0;
}

int run_render_frame(Collector& collector, const Hardware& hw, int W, int H,
                     unsigned long long warmup, const UiOptions& opts) {
  ColorMode mode;
  if (!resolve_color_mode(opts, mode)) return 2;
  if (W < 1 || H < 1) {
    std::fprintf(stderr, "geth: --render-frame size must be at least 1x1\n");
    return 2;
  }
  const Theme theme = Theme::cyberpunk_blue();
  const std::vector<PanelSpec> page = build_default_page(hw);
  FrameState state;

  // Collect warmup samples. Demo mode sleeps nowhere: instead each sample is
  // stamped on a VIRTUAL clock spaced by --interval, so 60 samples fill the
  // whole 60 s graph window (back-to-back wall-time samples would all land
  // within one millisecond and every graph column would show the same value;
  // the demo source follows out.time, so its waves advance too). Real
  // hardware sleeps the real interval.
  if (opts.demo) {
    TimePoint vnow = collector.latest().time;  // the probe's warm-up sample
    for (unsigned long long i = 0; i < warmup; ++i) {
      vnow += std::chrono::milliseconds(opts.interval_ms);
      collector.set_sample_time(vnow);
      collector.sample();
    }
  } else {
    for (unsigned long long i = 0; i < warmup; ++i) {
      collector.sample();
      if (i + 1 < warmup)
        std::this_thread::sleep_for(std::chrono::milliseconds(opts.interval_ms));
    }
  }

  FrameStats stats;
  const auto t0 = Clock::now();
  const long long a0 = alloc_count();
  ftxui::Screen screen(W, H);
  screen.Clear();
  LayoutCache cache;
  cache.page = &page;
  if (opts.columns > 0) cache.prefs.columns = opts.columns;
  const ftxui::Element el = build_frame_element(
      hw, page, collector.store(), collector.latest(), nullptr, state, theme,
      mode, /*paused=*/false, opts.interval_ms, cache,
      /*top_row=*/0, kWindowsNs[kDefaultWindow]);
  ftxui::Render(screen, el);
  stats.note_render(std::chrono::duration<double, std::micro>(Clock::now() - t0)
                        .count(),
                    alloc_count() - a0);

  const std::string text = screen_to_text(screen);

  // Geometry self-check: before anything is printed, verify the frame is
  // exactly H lines, each exactly W DISPLAY columns wide (wcwidth, not bytes
  // or code points). A layout bug must fail loudly, not print a broken frame.
  const GeometryReport geom = check_geometry(text, W);
  if (!geom.ok(H)) {
    std::fprintf(stderr,
                 "geth: geometry check failed at %dx%d: got %d lines; "
                 "wrong-width lines:",
                 W, H, geom.lines);
    for (int ln : geom.bad_lines) std::fprintf(stderr, " %d", ln);
    std::fprintf(stderr, "\n");
    return 1;
  }

  if (opts.ansi)
    std::fputs(screen.ToString().c_str(), stdout);  // SGR + style reset, no NUL
  else
    std::fputs(text.c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);

  if (opts.stats) stats.print();
  return 0;
}

}  // namespace geth::ui
