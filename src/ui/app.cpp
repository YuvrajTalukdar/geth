#include "ui/app.hpp"

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
                FrameStats& stats)
      : _collector(collector),
        _hw(hw),
        _page(page),
        _theme(theme),
        _mode(mode),
        _interval_ms(interval_ms),
        _count(count),
        _stats(stats) {}

  bool OnEvent(ftxui::Event event) override {
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
        _frozen = capture_frozen(_page, _collector.store(), _collector.latest());
      else
        _frozen.reset();
      return true;
    }
    return false;
  }

  ftxui::Element OnRender() override {
    const FrozenFrame* frozen = _paused ? &_frozen.value() : nullptr;
    const auto t0 = Clock::now();
    const long long a0 = alloc_count();
    auto el = build_frame_element(_hw, _page, _collector.store(),
                                  _collector.latest(), frozen, _state, _theme,
                                  _mode, _paused, _interval_ms);
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
      collector, hw, page, theme, mode, opts.interval_ms, opts.count, stats);

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

  // Collect warmup samples: no sleep in demo mode, the real interval
  // otherwise.
  for (unsigned long long i = 0; i < warmup; ++i) {
    collector.sample();
    if (!opts.demo && i + 1 < warmup)
      std::this_thread::sleep_for(std::chrono::milliseconds(opts.interval_ms));
  }

  FrameStats stats;
  const auto t0 = Clock::now();
  const long long a0 = alloc_count();
  ftxui::Screen screen(W, H);
  screen.Clear();
  const ftxui::Element el = build_frame_element(
      hw, page, collector.store(), collector.latest(), nullptr, state, theme,
      mode, /*paused=*/false, opts.interval_ms);
  ftxui::Render(screen, el);
  stats.note_render(std::chrono::duration<double, std::micro>(Clock::now() - t0)
                        .count(),
                    alloc_count() - a0);

  if (opts.ansi)
    std::fputs(screen.ToString().c_str(), stdout);  // SGR + style reset, no NUL
  else
    std::fputs(screen_to_text(screen).c_str(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);

  if (opts.stats) stats.print();
  return 0;
}

}  // namespace geth::ui
