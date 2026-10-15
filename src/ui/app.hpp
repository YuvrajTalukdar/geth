// The interactive UI (default mode) and the --render-frame dump.
#pragma once

#include "core/collector.hpp"
#include "ui/frame.hpp"
#include "ui/panel_spec.hpp"
#include "ui/theme.hpp"

namespace geth::ui {

struct UiOptions {
  long long interval_ms = 1000;
  unsigned long long count = 0;  // 0 = run until the user quits
  bool stats = false;            // print render/perf stats on exit
  std::string color_mode;        // "" = auto-detect, else truecolor|256|16
  bool demo = false;             // --demo: no sleep in --render-frame
  bool ansi = false;             // --render-frame: emit full SGR codes
};

// Validate `opts.color_mode` and resolve the effective color mode.
bool resolve_color_mode(const UiOptions& opts, ColorMode& out);

// Starts the interactive dashboard. Blocks until q / Esc / Ctrl-C (or, with
// count, that many ticks). Returns the process exit code.
int run_ui(Collector& collector, const Hardware& hw, const UiOptions& opts);

// --render-frame COLSxROWS: collects `warmup` samples (no sleep in demo mode,
// the real interval otherwise), renders exactly one frame at W x H to stdout
// and exits. Plain UTF-8 without `ansi`; full SGR escape codes with `ansi`.
int run_render_frame(Collector& collector, const Hardware& hw, int W, int H,
                     unsigned long long warmup, const UiOptions& opts);

}  // namespace geth::ui
