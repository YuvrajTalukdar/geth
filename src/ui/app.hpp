// The interactive UI (default mode), the --render-frame dump, and the Phase 4
// wiring: the config file (load/apply/save-on-change) and the Dashboard
// component — panel focus, the panel-manager overlay, the help overlay and
// toasts.
#pragma once

#include <atomic>
#include <map>
#include <string>
#include <vector>

#include "ftxui/component/component_base.hpp"
#include "ftxui/component/event.hpp"

#include "core/collector.hpp"
#include "ui/config.hpp"
#include "ui/frame.hpp"
#include "ui/panel_spec.hpp"
#include "ui/term_probe.hpp"
#include "ui/theme.hpp"

namespace geth::ui {

struct UiOptions {
  long long interval_ms = 1000;
  bool interval_explicit = false;  // --interval was passed; beats the config
  unsigned long long count = 0;    // 0 = run until the user quits
  bool stats = false;              // print render/perf stats on exit
  std::string color_mode;          // "" = auto-detect, else truecolor|256|16
  bool demo = false;               // --demo: no sleep in --render-frame
  bool ansi = false;               // --render-frame: emit full SGR codes
  int columns = 0;                 // --columns N: pin the column count (0 = auto)
  // Phase 4:
  std::string config_path;  // --config PATH ("" = the XDG default location)
  bool no_save = false;     // --no-save: the config file is read-only
  std::string script;       // --render-frame --script: comma-separated events
  // Phase 5:
  std::string theme;  // --theme NAME ("" = the config's [general] theme)
  // --renderer pixels|text ("" = the config's graph_renderer); a session
  // override, never saved.
  std::string renderer;
};

// Validate `opts.color_mode` and resolve the effective color mode.
bool resolve_color_mode(const UiOptions& opts, ColorMode& out);

// One panel-manager overlay row, in manager order: the present panels first
// (in page order), then the absent-hardware entries (in file order).
struct MgrRow {
  std::string id;
  std::string label;  // the (possibly renamed) title
  bool visible = true;
  bool present = true;  // false: hardware absent, dimmed "(not present)"
};

// The whole dashboard as ONE FTXUI component: OnEvent handles every key
// (sampling ticks, pause, focus, the manager, the help, settings, scrolling)
// and OnRender builds the frame. The config file lives here: the file is
// applied on startup (merge_page + the GPU overrides), and every setting
// change at runtime is saved atomically with a 2 s "saved" toast.
class Dashboard : public ftxui::ComponentBase {
 public:
  // `theme_cycle` (name, theme) is the full selectable list (built-ins +
  // custom files) in `T` cycle order; `theme_idx` the active one. The
  // Dashboard owns the copies (a borrowed theme would dangle after the
  // caller's scope ends).
  Dashboard(Collector& collector, Hardware hw, Config cfg,
            std::vector<std::pair<std::string, Theme>> theme_cycle,
            int theme_idx, ColorMode mode, long long interval_ms,
            unsigned long long count, int window_idx, int columns,
            FrameStats& stats);

  // The terminal size before the first resize event (--render-frame
  // --script and the tests): OnEvent needs it to clamp the scroll and map a
  // mouse click to a panel.
  void set_size(int w, int h) {
    _w = w;
    _h = h;
  }

  // -- state (unit tests) ------------------------------------------------------
  bool manager_open() const { return _manager_open; }
  bool help_open() const { return _help_open; }
  bool paused() const { return _paused; }
  int focus_index() const { return _focus; }  // a PAGE index, -1 = none
  const std::vector<PanelSpec>& page() const { return _page; }
  const Config& config() const { return _cfg; }
  const std::vector<std::string>& order() const { return _order; }
  const std::vector<MgrRow>& manager_rows() const { return _mg_rows; }
  int manager_selected() const { return _mg_sel; }
  int window_idx() const { return _window_idx; }
  int columns_setting() const { return _columns; }  // -1 = auto
  long long interval_ms() const { return _interval_ms.load(); }
  // The atomic itself (the timer thread sleeps on the CURRENT value, so a
  // `+`/`-` change takes effect on the next tick).
  std::atomic<long long>& interval() { return _interval_ms; }
  const std::string& toast() const { return _toast; }
  bool panel_visible(const std::string& id) const;
  int page_index(const std::string& id) const;  // -1 when not on the page
  // Phase 5: the active theme's name (T cycles it, each cycle saves it).
  std::string theme_name() const;
  // A startup warning shown as a toast (e.g. a broken custom-theme file).
  void note_warning(const std::string& msg);

  // The terminal capabilities, set by run_ui before the loop starts: the
  // probe result, the protocol the effective intent resolves to, and the
  // effective intent itself (the --renderer override when given, else the
  // config's graph_renderer).
  void set_probe(const ProbeResult& probe, ImageProto proto,
                 const std::string& effective_intent) {
    _probe = probe;
    _proto = proto;
    _renderer_eff = effective_intent;
    _pixels_layer = (proto != ImageProto::kNone);
  }
  // --render-frame has no probe: set the text layer's pixel flag directly
  // (the pixels text layer = graphs left as plain spaces).
  void set_pixels_layer(bool b) { _pixels_layer = b; }
  const ProbeResult& probe_result() const { return _probe; }
  ImageProto renderer_proto() const { return _proto; }
  const std::string& renderer_intent() const { return _renderer_eff; }

  bool OnEvent(ftxui::Event event) override;
  ftxui::Element OnRender() override;

 private:
  // The panel manager.
  void open_manager();
  void close_manager();
  bool manager_event(ftxui::Event ev);
  void rebuild_manager_rows();
  void clamp_manager_scroll();
  void keep_selection_visible();
  int sub_row_count() const;
  void toggle_row(int i);
  void move_row(int i, int dir);
  void commit_rename();
  void do_reset();

  // Focus.
  void focus_next(int dir);
  void hide_focused();
  void focus_at(int x, int y);

  // Settings (each saves with a "saved" toast).
  void cycle_interval(int dir);
  void cycle_window();
  void cycle_columns();
  void cycle_theme();
  void cycle_graph_renderer();

  // Rebuilds the page from _hw (the GPU overrides are already applied in it)
  // and _cfg, keeping the manager's order; run after a global GPU rename or
  // a reset, so every GPU-derived title/legend follows the new name.
  void rebuild_page();

  // The config file.
  void save_now();
  void toast(const std::string& msg);
  int count_visible() const;
  std::string default_title(const std::string& id) const;

  // The active theme (the `T` cycle's current entry).
  const Theme& theme() const;

  // The manager overlay box at the current terminal size — the SAME math as
  // OverlayBox::make(W, H, 60, min(H-4, 30), 2) in frame.cpp, so a mouse
  // click maps to the row the renderer drew.
  struct MgrBox {
    int x0 = 0, y0 = 0, w = 0, h = 0, row_count = 0;
  };
  MgrBox manager_box() const;

  Collector& _collector;
  Hardware _hw;  // owned: the [gpus."..."] overrides are applied here
  Config _cfg;   // owned: the file the UI reads and (unless --no-save) saves
  std::vector<std::pair<std::string, Theme>> _theme_cycle;  // Phase 5: T cycles
  int _theme_idx = 0;
  // The detected GPU identity BEFORE the overrides are applied — the target
  // a rename-clear or a reset restores to. Keyed by PCI bus id.
  struct GpuOrig {
    std::string name;
    bool integrated = false;
  };
  std::map<std::string, GpuOrig> _gpu_orig;
  ColorMode _mode;
  std::atomic<long long> _interval_ms;
  unsigned long long _count;
  FrameStats& _stats;

  std::vector<PanelSpec> _page;  // the PRESENT panels, in _order position
  std::vector<std::string> _order;  // present + absent ids, manager order
  std::map<std::string, std::string> _default_titles;  // id -> default title

  LayoutCache _cache;
  int _w = 0, _h = 0;
  int _top_row = 0;
  int _focus = -1;
  int _window_idx = 1;
  int _columns = -1;  // -1 = auto
  bool _mouse_tracked = true;  // FTXUI's default: mouse tracking ON

  FrameState _state;
  bool _paused = false;
  std::optional<FrozenFrame> _frozen;
  unsigned long long _ticks = 0;

  std::string _toast;
  TimePoint _toast_until{};
  bool _dirty = false;  // a change not yet saved

  // The panel manager.
  bool _manager_open = false;
  std::vector<MgrRow> _mg_rows;
  int _mg_sel = 0;
  int _mg_scroll = 0;
  bool _mg_renaming = false;
  std::string _mg_rename_buf;
  bool _mg_submenu = false;
  int _mg_sub_sel = 0;
  bool _mg_confirm_reset = false;

  bool _help_open = false;

  // The pixel renderer (v1.1.0): the startup probe and what it resolved to.
  ProbeResult _probe;
  ImageProto _proto = ImageProto::kNone;
  // The effective renderer intent at startup ("" = auto): the --renderer
  // override when given, else the config's graph_renderer. `g` moves it to
  // the other available renderer and saves that.
  std::string _renderer_eff = "auto";
  // What the TEXT layer draws: true = the resolved renderer is pixels, so
  // the graph rects are left plain spaces for the pixel backend.
  bool _pixels_layer = false;
};

// Parse one --script token into a synthetic FTXUI Event:
//   space, enter, esc, tab, shift-tab, up, down, left, right, pageup,
//   pagedown, home, end, backspace, "click:X:Y" (a left-button press at
//   X,Y), or a single character typed as-is. false on anything else.
bool parse_script_token(const std::string& tok, ftxui::Event& out);

// Starts the interactive dashboard. Blocks until q / Esc / Ctrl-C (or, with
// count, that many ticks). Returns the process exit code.
int run_ui(Collector& collector, const Hardware& hw, const UiOptions& opts);

// --render-frame COLSxROWS: collects `warmup` samples (no sleep in demo
// mode, the real interval otherwise), replays the optional --script, renders
// exactly one frame at W x H to stdout and exits. Plain UTF-8 without `ansi`;
// full SGR escape codes with `ansi`.
int run_render_frame(Collector& collector, const Hardware& hw, int W, int H,
                     unsigned long long warmup, const UiOptions& opts);

}  // namespace geth::ui
