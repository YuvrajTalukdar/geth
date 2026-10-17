#include "ui/app.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <thread>

#include "ftxui/component/component.hpp"
#include "ftxui/component/screen_interactive.hpp"

#include "util/alloc_count.hpp"
#include "util/stop.hpp"
#include "util/terminal.hpp"

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

// The graph time window, cycled by `t`: 30 s -> 1 m -> 2 m -> 5 m -> 10 m.
// Index 1 (1 m) is the default, the pre-Phase-3 fixed 60 s window.
constexpr int64_t kWindowsNs[] = {
    30LL * 1000000000LL,    // 30 s
    60LL * 1000000000LL,    // 1 m
    120LL * 1000000000LL,   // 2 m
    300LL * 1000000000LL,   // 5 m
    600LL * 1000000000LL,   // 10 m
};
constexpr int kNumWindows = 5;
constexpr int kDefaultWindow = 1;  // 1 m

// The intervals `+` / `-` cycle through (the config's allowed set).
const long long kIntervalsMs[] = {250, 500, 1000, 2000, 5000};
constexpr int kNumIntervals = 5;

// The settings the UI actually runs with: the CLI flag beats the config, the
// config beats the default, and invalid values fall back to the default.
struct Effective {
  long long interval_ms = 1000;
  int window_idx = kDefaultWindow;
  int columns = -1;  // -1 = auto
};
Effective effective_settings(const UiOptions& opts, const Config& cfg) {
  Effective e;
  e.interval_ms = opts.interval_explicit ? opts.interval_ms
                 : (valid_interval_ms(cfg.interval_ms) ? cfg.interval_ms : 1000);
  for (int i = 0; i < kNumWindows; ++i)
    if (cfg.window_ns == kWindowsNs[i]) e.window_idx = i;
  e.columns = (opts.columns > 0) ? opts.columns
             : (valid_columns(cfg.columns) ? cfg.columns : -1);
  return e;
}

}  // namespace

// ---------------------------------------------------------------------------
// Dashboard
// ---------------------------------------------------------------------------

Dashboard::Dashboard(Collector& collector, Hardware hw, Config cfg,
                     std::vector<std::pair<std::string, Theme>> theme_cycle,
                     int theme_idx, ColorMode mode, long long interval_ms,
                     unsigned long long count, int window_idx, int columns,
                     FrameStats& stats)
    : _collector(collector),
      _hw(std::move(hw)),
      _cfg(std::move(cfg)),
      _theme_cycle(std::move(theme_cycle)),
      _theme_idx(_theme_cycle.empty()
                     ? 0
                     : std::clamp(theme_idx, 0, (int)_theme_cycle.size() - 1)),
      _mode(mode),
      _interval_ms(interval_ms),
      _count(count),
      _stats(stats) {
  _window_idx = std::clamp(window_idx, 0, kNumWindows - 1);
  _columns = (columns > 0) ? columns : -1;

  // The detected GPU identity BEFORE the overrides are applied: a rename
  // cleared in the manager (or a reset) restores these.
  for (const auto& g : _hw.gpus)
    _gpu_orig[g.bus_id] = GpuOrig{g.name, g.is_integrated};
  // The [gpus."..."] overrides (name / integrated) land in _hw; every
  // GPU-derived title flows from it.
  apply_gpu_overrides(_hw, _cfg);

  // The manager's order: the present panels (default page order), then the
  // absent-hardware entries (file order).
  const std::vector<PanelSpec> default_page = build_default_page(_hw);
  _order.clear();
  _order.reserve(default_page.size() + _cfg.panels.size());
  for (const auto& p : default_page) _order.push_back(p.id);
  std::set<std::string> present;
  for (const auto& p : default_page) present.insert(p.id);
  for (const auto& e : _cfg.panels)
    if (!present.count(e.id)) _order.push_back(e.id);

  // The merged page: the config's order/settings on the defaults, new
  // hardware inserted VISIBLE at its default position, and panels whose
  // hardware is absent dropped from the page (they stay in the file and show
  // up in the manager as "(not present)").
  rebuild_page();

  _cache.page = &_page;
  if (_columns > 0) _cache.prefs.columns = _columns;
}

const Theme& Dashboard::theme() const {
  static const Theme kFallback = Theme::cyberpunk_blue();
  return _theme_cycle.empty() ? kFallback : _theme_cycle[_theme_idx].second;
}

std::string Dashboard::theme_name() const {
  if (_theme_cycle.empty()) return "cyberpunk-blue";
  return _theme_cycle[_theme_idx].first;
}

void Dashboard::note_warning(const std::string& msg) {
  _toast = msg;
  _toast_until = Clock::now() + std::chrono::seconds(5);
}

void Dashboard::rebuild_page() {
  const std::vector<PanelSpec> def = build_default_page(_hw);
  for (const auto& p : def) _default_titles[p.id] = p.title;
  std::vector<PanelSpec> page = merge_page(def, _cfg);
  // Keep the manager's order (_order): merge_page returns the FILE's order,
  // which equals _order only while every change has been saved (no-save
  // sessions keep _order in memory).
  std::map<std::string, PanelSpec> by_id;
  for (auto& p : page) by_id[p.id] = std::move(p);
  page.clear();
  for (const auto& id : _order)
    if (auto it = by_id.find(id); it != by_id.end())
      page.push_back(std::move(it->second));
  _page = std::move(page);
  _cache.invalidate();
}

bool Dashboard::OnEvent(ftxui::Event event) {
  if (event.screen_) {
    _w = event.screen_->dimx();
    _h = event.screen_->dimy();
  }
  // Every event goes through the layout cache first: get() recomputes ONLY
  // when the terminal size changed since the last call. Keep mouse tracking
  // in step with the overflow state and clamp the scroll position.
  const Layout* lo = nullptr;
  if (_w > 0 && _h > 0) {
    lo = &_cache.get(_w, _h);
    if (lo->scrolling != _mouse_tracked) {
      if (event.screen_) event.screen_->TrackMouse(lo->scrolling);
      _mouse_tracked = lo->scrolling;
    }
    _top_row = std::clamp(_top_row, 0, max_top_row(*lo, _h - 2));
    if (_manager_open) clamp_manager_scroll();
  }

  // Tick from the timer thread: sampling ALWAYS continues (even while paused
  // — "Sampling continues; the display freezes").
  if (event == ftxui::Event::Custom) {
    const auto t0 = Clock::now();
    _collector.sample();
    _stats.note_sample(
        std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
    if (++_ticks >= _count && _count != 0 && event.screen_)
      event.screen_->Exit();
    return true;
  }

  // The help overlay: ANY key closes it.
  if (_help_open) {
    _help_open = false;
    return true;
  }

  // The panel manager swallows everything while it is open.
  if (_manager_open) return manager_event(event);

  // Quit: save any pending change first.
  if (event == ftxui::Event::Character('q') ||
      event == ftxui::Event::Character('Q') || event == ftxui::Event::Escape ||
      event == ftxui::Event::CtrlC) {
    if (_dirty) save_now();
    // FTXUI restores the terminal (alternate screen, termios, cursor) on
    // Exit, including when the loop ends via a crash signal.
    if (event.screen_) event.screen_->Exit();
    return true;
  }

  // Pause: freeze the display from a capture; sampling continues.
  if (event == ftxui::Event::Character(' ')) {
    _paused = !_paused;
    if (_paused)
      _frozen = capture_frozen(_page, _collector.store(), _collector.latest(),
                               kWindowsNs[_window_idx]);
    else
      _frozen.reset();
    return true;
  }

  // Focus (Tab / Shift-Tab wrap over the VISIBLE panels, x hides, click
  // focuses).
  if (event == ftxui::Event::Tab) {
    focus_next(+1);
    return true;
  }
  if (event == ftxui::Event::TabReverse) {
    focus_next(-1);
    return true;
  }
  if (event == ftxui::Event::Character('x')) {
    hide_focused();
    return true;
  }

  // Overlays.
  if (event == ftxui::Event::Character('e')) {
    open_manager();
    return true;
  }
  if (event == ftxui::Event::Character('?')) {
    _help_open = true;
    return true;
  }

  // Settings (each change saves the config with a "saved" toast).
  if (event == ftxui::Event::Character('+') ||
      event == ftxui::Event::Character('='))
    return (cycle_interval(+1), true);
  if (event == ftxui::Event::Character('-') ||
      event == ftxui::Event::Character('_'))
    return (cycle_interval(-1), true);
  if (event == ftxui::Event::Character('t')) return (cycle_window(), true);
  if (event == ftxui::Event::Character('c')) return (cycle_columns(), true);
  if (event == ftxui::Event::Character('T')) return (cycle_theme(), true);

  // Scrolling (no-ops unless the page overflows).
  auto line_scroll = [&](int dir) {
    if (!lo || !lo->scrolling) return;
    _top_row = std::clamp(_top_row + dir, 0, max_top_row(*lo, _h - 2));
  };
  auto page_scroll = [&](int dir) {
    if (!lo || !lo->scrolling) return;
    _top_row = std::clamp(_top_row + dir * rows_fitting(*lo, _h - 2), 0,
                          max_top_row(*lo, _h - 2));
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
    if (lo) _top_row = max_top_row(*lo, _h - 2);
    return true;
  }

  // Mouse: the wheel scrolls (only while the page overflows), a left click
  // focuses the panel under the cursor.
  if (event.is_mouse()) {
    const ftxui::Mouse& m = event.mouse();
    if (m.button == ftxui::Mouse::WheelUp && m.motion == ftxui::Mouse::Pressed)
      return (line_scroll(-1), true);
    if (m.button == ftxui::Mouse::WheelDown &&
        m.motion == ftxui::Mouse::Pressed)
      return (line_scroll(1), true);
    if (m.button == ftxui::Mouse::Left && m.motion == ftxui::Mouse::Pressed) {
      focus_at(m.x, m.y);
      return true;
    }
  }
  return false;
}

ftxui::Element Dashboard::OnRender() {
  const FrozenFrame* frozen = _paused ? &_frozen.value() : nullptr;
  const auto t0 = Clock::now();
  const long long a0 = alloc_count();

  FrameChrome chrome;
  chrome.config_warning = _cfg.loaded ? "" : _cfg.error;
  chrome.toast = _toast;
  chrome.toast_until = _toast_until;
  chrome.title_override = _cfg.title;
  chrome.solid_bg = (_cfg.background == "solid");

  ManagerOverlay mg;
  bool show_manager = false;
  if (_manager_open) {
    show_manager = true;
    mg.rows.resize(_mg_rows.size());
    for (int i = 0; i < (int)_mg_rows.size(); ++i) {
      mg.rows[i].label = _mg_rows[i].label;
      mg.rows[i].visible = _mg_rows[i].visible;
      mg.rows[i].present = _mg_rows[i].present;
      mg.rows[i].selected = (i == _mg_sel);
    }
    mg.scroll = _mg_scroll;
    mg.renaming = _mg_renaming;
    mg.rename_row = _mg_sel;
    mg.rename_buf = _mg_rename_buf;
    mg.submenu = _mg_submenu;
    mg.confirm_reset = _mg_confirm_reset;
    if (_mg_submenu && _mg_sel >= 0 && _mg_sel < (int)_page.size()) {
      const PanelSpec& p = _page[_mg_sel];
      mg.submenu_title = "Series \xE2\x80\x94 " + p.title;
      mg.sub_rows.clear();
      for (std::size_t s = 0; s < p.series.size(); ++s)
        mg.sub_rows.push_back({p.series[s].label, p.series[s].hidden,
                               (int)s == _mg_sub_sel});
    }
  }

  HelpOverlay help;
  bool show_help = false;
  if (_help_open) {
    show_help = true;
    help.lines = {
        {"q / Esc", "quit"},
        {"e", "panel manager"},
        {"? / Esc", "this help (any key closes)"},
        {"↑↓/j k · space", "manager: select / toggle a panel"},
        {"K/J (in manager)", "move a panel up / down"},
        {"r / enter / d (in manager)", "rename / series submenu / reset"},
        {"space (in submenu)", "hide / show a series"},
        {"space", "pause / resume (sampling continues)"},
        {"Tab / Shift-Tab", "focus next / previous panel"},
        {"x", "hide the focused panel"},
        {"click", "focus a panel"},
        {"+ / =", "faster interval (250 ms \xE2\x80\xA6 5 s)"},
        {"- / _", "slower interval"},
        {"t", "cycle the graph window (30 s \xE2\x80\xA6 10 m)"},
        {"c", "cycle the columns (auto, 1 \xE2\x80\xA6 8)"},
        {"T", "cycle the theme (saves)"},
        {"\xE2\x86\x91 / \xE2\x86\x93 \xC2\xB7 j / k", "scroll one row"},
        {"PgUp / PgDn", "scroll one page"},
        {"Home / End", "top / bottom"},
    };
  }

  auto el = build_frame_element(_hw, _page, _collector.store(),
                                _collector.latest(), frozen, _state, theme(),
                                _mode, _paused, _interval_ms.load(), _cache,
                                _top_row, kWindowsNs[_window_idx], chrome,
                                _focus, show_manager ? &mg : nullptr,
                                show_help ? &help : nullptr);
  _stats.note_render(
      std::chrono::duration<double, std::micro>(Clock::now() - t0).count(),
      alloc_count() - a0);
  return el;
}

// -- Focus --------------------------------------------------------------------

void Dashboard::focus_next(int dir) {
  std::vector<int> vis;
  for (int i = 0; i < (int)_page.size(); ++i)
    if (_page[i].visible) vis.push_back(i);
  if (vis.empty()) {
    _focus = -1;
    return;
  }
  bool found = false;
  for (int i : vis)
    if (i == _focus) {
      found = true;
      break;
    }
  if (!found) {  // nothing focused yet: select an end
    _focus = (dir > 0) ? vis.front() : vis.back();
    return;
  }
  for (std::size_t i = 0; i < vis.size(); ++i)
    if (vis[i] == _focus) {
      _focus = vis[(i + (dir > 0 ? 1 : vis.size() - 1)) % vis.size()];
      return;
    }
}

void Dashboard::hide_focused() {
  if (_focus < 0 || _focus >= (int)_page.size() || !_page[_focus].visible) {
    // No valid focus: just select the first visible panel (the next `x`
    // hides it).
    focus_next(+1);
    return;
  }
  const int hidden = _focus;
  if (count_visible() == 1) {
    toast("at least one panel must stay visible");
    return;
  }
  _page[hidden].visible = false;
  _dirty = true;
  _cache.invalidate();
  // Advance to the NEXT visible panel after the one hidden, wrapping.
  _focus = -1;
  for (int step = 1; step <= (int)_page.size(); ++step) {
    const int cand = (hidden + step) % (int)_page.size();
    if (_page[cand].visible) {
      _focus = cand;
      break;
    }
  }
  save_now();
}

void Dashboard::focus_at(int x, int y) {
  if (_w < 1 || _h < 1) return;
  const Layout& lo = _cache.get(_w, _h);
  if (lo.panels.empty()) return;
  const int tr =
      lo.scrolling ? std::clamp(_top_row, 0, max_top_row(lo, _h - 2)) : 0;
  const int last_visible =
      lo.scrolling ? std::min(lo.total_rows, tr + lo.rows()) : lo.rows();
  // The on-screen position of a visible panel: when not scrolling the stored
  // rect is already screen-ready (rect.y is content-relative, offset 0). When
  // scrolling, the stored rect is the offset-0 reference form, so the y and
  // height are recomputed from the k stretched visible rows — exactly as
  // render_frame does (the stored rect is never drawn in scroll mode).
  for (int r = tr; r < last_visible; ++r)
    for (int i = 0; i < (int)lo.panels.size(); ++i) {
      const LayoutRect& rc = lo.panels[i].rect;
      if (rc.row != r) continue;
      int py = 1 + rc.y;
      int ph = rc.h;
      if (lo.scrolling) {
        const int vis = r - tr;  // 0 .. k-1 within the visible window
        int top = 1;
        for (int q = 0; q < vis; ++q) top += lo.row_heights[q];
        py = top;
        ph = lo.row_heights[vis];
      }
      if (x >= rc.x && x < rc.x + rc.w && y >= py && y < py + ph) {
        _focus = _cache.visible_idx[i];
        return;
      }
    }
}

// -- Settings -----------------------------------------------------------------

void Dashboard::cycle_interval(int dir) {
  // dir = +1: FASTER (smaller ms, 1000 -> 500); -1: slower (1000 -> 2000).
  // The presets are ascending, so "faster" is a step to a LOWER index.
  const long long cur = _interval_ms.load();
  int new_idx = -1;
  bool exact = false;
  for (int i = 0; i < kNumIntervals; ++i)
    if (kIntervalsMs[i] == cur) {
      new_idx = (i - dir + kNumIntervals) % kNumIntervals;
      exact = true;
    }
  if (!exact) {
    // A non-preset interval (e.g. --interval 300): step to the NEAREST
    // preset in the requested direction.
    if (dir > 0) {
      // the largest preset below cur; when there is none, wrap to the top
      new_idx = kNumIntervals - 1;
      for (int i = 0; i < kNumIntervals; ++i)
        if (kIntervalsMs[i] < cur) new_idx = i;
    } else {
      // the smallest preset above cur; when there is none, wrap to the
      // bottom
      new_idx = 0;
      for (int i = 0; i < kNumIntervals; ++i)
        if (kIntervalsMs[i] > cur) {
          new_idx = i;
          break;
        }
    }
  }
  _interval_ms.store(kIntervalsMs[new_idx]);
  _collector.set_interval_ms(kIntervalsMs[new_idx]);  // sizes the gap threshold
  _dirty = true;
  save_now();
}

void Dashboard::cycle_window() {
  _window_idx = (_window_idx + 1) % kNumWindows;
  _dirty = true;
  save_now();
}

void Dashboard::cycle_columns() {
  // auto -> 1 -> 2 -> ... -> 8 -> auto
  const int cur = (_columns < 0) ? 0 : _columns;
  const int nxt = (cur + 1) % 9;
  _columns = (nxt == 0) ? -1 : nxt;
  if (_columns < 0)
    _cache.prefs.columns.reset();
  else
    _cache.prefs.columns = _columns;
  _cache.invalidate();
  _dirty = true;
  save_now();
}

void Dashboard::cycle_theme() {
  // T cycles the selectable themes (built-ins + custom files) and saves the
  // new choice; with a single theme it just persists the (same) choice.
  if (_theme_cycle.empty()) return;
  if (_theme_cycle.size() > 1)
    _theme_idx = (_theme_idx + 1) % (int)_theme_cycle.size();
  _dirty = true;
  save_now();
}

// -- The panel manager ---------------------------------------------------------

void Dashboard::open_manager() {
  _manager_open = true;
  _mg_renaming = _mg_submenu = _mg_confirm_reset = false;
  rebuild_manager_rows();
  // Start on the focused panel's row when there is one (present rows are the
  // page, in order, at the front).
  _mg_sel = (_focus >= 0 && _focus < (int)_page.size()) ? _focus : 0;
  _mg_scroll = 0;
  keep_selection_visible();
}

void Dashboard::close_manager() {
  const bool had_selection =
      _mg_sel >= 0 && _mg_sel < (int)_mg_rows.size() && _mg_rows[_mg_sel].present;
  const int sel = _mg_sel;
  _manager_open = false;
  _mg_renaming = _mg_submenu = _mg_confirm_reset = false;
  _focus = had_selection ? sel : -1;
  if (_dirty) save_now();  // close saves
}

void Dashboard::rebuild_manager_rows() {
  _mg_rows.clear();
  _mg_rows.reserve(_order.size());
  std::set<std::string> present;
  for (const auto& p : _page) present.insert(p.id);
  for (const auto& id : _order) {
    MgrRow r;
    r.id = id;
    const PanelSpec* p = nullptr;
    for (const auto& ps : _page)
      if (ps.id == id) {
        p = &ps;
        break;
      }
    if (p) {
      r.label = p->title;
      r.visible = p->visible;
      r.present = true;
    } else {
      r.label = id;
      r.visible = true;
      for (const auto& e : _cfg.panels)
        if (e.id == id) {
          if (!e.title.empty()) r.label = e.title;
          r.visible = e.visible;
          break;
        }
      r.present = false;
    }
    _mg_rows.push_back(std::move(r));
  }
  if (_mg_sel >= (int)_mg_rows.size()) _mg_sel = (int)_mg_rows.size() - 1;
  clamp_manager_scroll();
}

Dashboard::MgrBox Dashboard::manager_box() const {
  MgrBox b;
  if (_w < 1 || _h < 1) return b;
  // Same computation as OverlayBox::make(W, H, 60, min(H-4, 30), 2).
  b.w = std::min(60, _w - 4);
  if (b.w < 24) b.w = std::min(_w, 60);
  b.h = std::min(_h - 4, 30);
  if (b.h < 8) b.h = _h;
  b.x0 = (_w - b.w) / 2;
  b.y0 = (_h - b.h) / 2;
  b.row_count = std::max(0, b.h - 3);  // interior: box_h - 3 rows
  return b;
}

void Dashboard::clamp_manager_scroll() {
  const MgrBox box = manager_box();
  const int n = (int)_mg_rows.size();
  const int maxs = box.row_count > 0 ? std::max(0, n - box.row_count) : 0;
  _mg_scroll = std::clamp(_mg_scroll, 0, maxs);
}

void Dashboard::keep_selection_visible() {
  const MgrBox box = manager_box();
  if (box.row_count <= 0) return;
  if (_mg_sel < _mg_scroll) _mg_scroll = _mg_sel;
  if (_mg_sel >= _mg_scroll + box.row_count)
    _mg_scroll = _mg_sel - box.row_count + 1;
  clamp_manager_scroll();
}

int Dashboard::sub_row_count() const {
  if (_mg_sel < 0 || _mg_sel >= (int)_mg_rows.size()) return 0;
  if (!_mg_rows[_mg_sel].present) return 0;
  if (_mg_sel >= (int)_page.size()) return 0;
  return (int)_page[_mg_sel].series.size();
}

bool Dashboard::manager_event(ftxui::Event ev) {
  // Inline rename first: it owns Esc (cancel the rename, not close the
  // manager), Enter, Backspace and every typed character.
  if (_mg_renaming) {
    if (ev == ftxui::Event::Escape) {
      _mg_renaming = false;
      return true;
    }
    if (ev == ftxui::Event::Return) {
      commit_rename();
      return true;
    }
    if (ev == ftxui::Event::Backspace) {
      if (!_mg_rename_buf.empty()) _mg_rename_buf.pop_back();
      return true;
    }
    if (ev.is_character()) {
      const std::string c = ev.character();
      if (c.size() == 1 && (unsigned char)c[0] >= 0x20) _mg_rename_buf += c;
      return true;
    }
    return true;
  }

  // The series submenu of the selected panel. Esc closes only the submenu
  // (a second Esc closes the manager); every other key is swallowed.
  if (_mg_submenu) {
    if (ev == ftxui::Event::Escape) {
      _mg_submenu = false;
      return true;
    }
    const int n = sub_row_count();
    if (n > 0) {
      if (ev == ftxui::Event::ArrowUp || ev == ftxui::Event::Character('k'))
        _mg_sub_sel = std::max(0, _mg_sub_sel - 1);
      if (ev == ftxui::Event::ArrowDown ||
          ev == ftxui::Event::Character('j'))
        _mg_sub_sel = std::min(n - 1, _mg_sub_sel + 1);
      if (ev == ftxui::Event::Character(' ')) {
        const int pi = _mg_sel;  // a present row IS a page index
        auto& sr = _page[pi].series[_mg_sub_sel];
        sr.hidden = !sr.hidden;
        _dirty = true;
        _cache.invalidate();
        save_now();
      }
    }
    return true;
  }

  // The "reset panels to defaults?" confirmation: y accepts, any other key
  // (Esc included) cancels.
  if (_mg_confirm_reset) {
    if (ev == ftxui::Event::Character('y') ||
        ev == ftxui::Event::Character('Y'))
      do_reset();
    _mg_confirm_reset = false;
    return true;
  }

  if (ev == ftxui::Event::Escape || ev == ftxui::Event::Character('e'))
    return (close_manager(), true);
  if (_mg_rows.empty()) return true;

  const int n = (int)_mg_rows.size();
  if (ev == ftxui::Event::ArrowUp || ev == ftxui::Event::Character('k')) {
    _mg_sel = std::max(0, _mg_sel - 1);
    keep_selection_visible();
    return true;
  }
  if (ev == ftxui::Event::ArrowDown || ev == ftxui::Event::Character('j')) {
    _mg_sel = std::min(n - 1, _mg_sel + 1);
    keep_selection_visible();
    return true;
  }
  if (ev == ftxui::Event::Character(' ')) {
    toggle_row(_mg_sel);
    return true;
  }
  if (ev == ftxui::Event::Character('K') ||
      ev == ftxui::Event::ArrowUpCtrl)
    return (move_row(_mg_sel, -1), true);
  if (ev == ftxui::Event::Character('J') ||
      ev == ftxui::Event::ArrowDownCtrl)
    return (move_row(_mg_sel, +1), true);
  if (ev == ftxui::Event::Character('r')) {
    _mg_renaming = true;
    _mg_rename_buf = _mg_rows[_mg_sel].label;
    return true;
  }
  if (ev == ftxui::Event::Return) {
    if (_mg_rows[_mg_sel].present) {
      _mg_submenu = true;
      _mg_sub_sel = 0;
    }
    return true;
  }
  if (ev == ftxui::Event::Character('d')) {
    _mg_confirm_reset = true;
    return true;
  }
  // Mouse: wheel scrolls, a click selects (a click on the checkbox also
  // toggles).
  if (ev.is_mouse()) {
    const ftxui::Mouse& m = ev.mouse();
    if (m.motion == ftxui::Mouse::Pressed) {
      if (m.button == ftxui::Mouse::WheelUp) {
        _mg_scroll = std::max(0, _mg_scroll - 1);
        return true;
      }
      if (m.button == ftxui::Mouse::WheelDown) {
        ++_mg_scroll;
        clamp_manager_scroll();
        return true;
      }
      if (m.button == ftxui::Mouse::Left) {
        const MgrBox box = manager_box();
        const int ry = m.y - (box.y0 + 1);
        if (ry >= 0 && ry < box.row_count) {
          const int idx = std::clamp(_mg_scroll + ry, 0, n - 1);
          const bool on_checkbox = (m.x >= box.x0 + 2 && m.x < box.x0 + 4);
          _mg_sel = idx;
          keep_selection_visible();
          if (on_checkbox) toggle_row(idx);
          return true;
        }
      }
    }
    return true;
  }
  return true;  // everything else is swallowed while the manager is open
}

void Dashboard::toggle_row(int i) {
  if (i < 0 || i >= (int)_mg_rows.size()) return;
  MgrRow& r = _mg_rows[i];
  if (r.present) {
    // Present rows are the page, in order: the row index IS the page index.
    if (i >= (int)_page.size()) return;
    if (_page[i].visible && count_visible() == 1) {
      toast("at least one panel must stay visible");
      return;
    }
    _page[i].visible = !_page[i].visible;
    _cache.invalidate();
  } else {
    // Absent hardware: the setting lives on the config entry (for the day
    // the hardware shows up).
    for (auto& e : _cfg.panels)
      if (e.id == r.id) {
        e.visible = !e.visible;
        break;
      }
  }
  _dirty = true;
  rebuild_manager_rows();
  save_now();
}

void Dashboard::move_row(int i, int dir) {
  const int j = i + dir;
  if (j < 0 || j >= (int)_order.size()) return;
  std::swap(_order[i], _order[j]);
  // Re-sync the page so the present panels follow the new order.
  std::map<std::string, PanelSpec> by_id;
  for (auto& p : _page) by_id[p.id] = std::move(p);
  _page.clear();
  for (const auto& id : _order)
    if (auto it = by_id.find(id); it != by_id.end())
      _page.push_back(std::move(it->second));
  _dirty = true;
  _cache.invalidate();
  rebuild_manager_rows();
  _mg_sel = j;  // the selection follows the moved row
  keep_selection_visible();
  save_now();
}

void Dashboard::commit_rename() {
  _mg_renaming = false;
  const int i = _mg_sel;
  if (i < 0 || i >= (int)_mg_rows.size()) return;
  const MgrRow r = _mg_rows[i];  // copy before the rebuild
  std::string label = _mg_rename_buf;
  while (!label.empty() && label.back() == ' ') label.pop_back();
  if (r.present) {
    // A GPU panel (usage or memory) renames the GPU GLOBALLY: the name lands
    // in _hw (every GPU-derived title, the temps/power legends, the compact
    // titles and the auto header title flow from it) and is persisted as
    // [gpus."bus"].name. A per-panel `title` set separately still wins for
    // its own panel (merge_page re-applies it on top).
    std::string bus, suffix;
    if (r.id.rfind("gpu.", 0) == 0) {
      const std::string rest = r.id.substr(4);
      const auto dot = rest.rfind('.');
      if (dot != std::string::npos) {
        bus = rest.substr(0, dot);
        suffix = rest.substr(dot + 1);
      }
    }
    int gi = -1;
    if (suffix == "usage" || suffix == "vram")
      for (int k = 0; k < (int)_hw.gpus.size(); ++k)
        if (_hw.gpus[k].bus_id == bus) gi = k;
    if (gi >= 0) {
      auto& g = _hw.gpus[gi];
      if (label.empty()) {
        // Clear the rename: restore the detected name.
        if (const auto it = _gpu_orig.find(g.bus_id); it != _gpu_orig.end())
          g.name = it->second.name;
        g.renamed = false;
        if (auto it = _cfg.gpus.find(g.bus_id); it != _cfg.gpus.end())
          it->second.name.clear();
      } else {
        g.name = label;
        g.renamed = true;
        _cfg.gpus[g.bus_id].name = label;
      }
      rebuild_page();  // every GPU title/legend follows the new name
    } else {
      if (i < (int)_page.size())
        _page[i].title = label.empty() ? default_title(r.id) : label;
      _cache.invalidate();
    }
  } else {
    for (auto& e : _cfg.panels)
      if (e.id == r.id) {
        e.title = label;  // "" = the default title
        break;
      }
  }
  _dirty = true;
  rebuild_manager_rows();
  save_now();
}

void Dashboard::do_reset() {
  // Reset EVERYTHING: the panel settings AND the per-GPU overrides (a rename,
  // an integrated override) — the page returns to the detected hardware.
  for (auto& [bus, ge] : _cfg.gpus) {
    ge.name.clear();
    ge.integrated.reset();
  }
  for (auto& g : _hw.gpus)
    if (const auto it = _gpu_orig.find(g.bus_id); it != _gpu_orig.end()) {
      g.name = it->second.name;
      g.is_integrated = it->second.integrated;
      g.renamed = false;
    }
  _page = build_default_page(_hw);
  for (auto& e : _cfg.panels) {
    e.visible = true;
    e.title.clear();
    e.hidden_series.clear();
  }
  _order.clear();
  for (const auto& p : _page) _order.push_back(p.id);
  std::set<std::string> present;
  for (const auto& p : _page) present.insert(p.id);
  for (const auto& e : _cfg.panels)
    if (!present.count(e.id)) _order.push_back(e.id);
  _focus = -1;
  _mg_sel = 0;
  _mg_scroll = 0;
  _cache.invalidate();
  rebuild_manager_rows();
  _dirty = true;
  save_now();
}

// -- The config file -----------------------------------------------------------

void Dashboard::save_now() {
  // The in-memory config is ALWAYS synced to the current runtime state, so
  // config() is a faithful representation even under --no-save (where only
  // the file write is suppressed).
  if (valid_interval_ms(_interval_ms.load()))
    _cfg.interval_ms = _interval_ms.load();
  _cfg.window_ns = kWindowsNs[_window_idx];
  _cfg.columns = _columns;
  // The active theme (always a known name: it is an entry of _theme_cycle).
  _cfg.theme = theme_name();
  _cfg.sync_panels(_page, _order);
  // A panel at its default title writes an EMPTY title (the config's "the
  // default").
  for (auto& e : _cfg.panels) {
    const std::string def = default_title(e.id);
    for (const auto& p : _page)
      if (p.id == e.id && p.title == def) {
        e.title.clear();
        break;
      }
  }
  // --no-save: the file is read-only (no write, no toast). An unparseable
  // file is NEVER overwritten.
  if (_cfg.no_save || !_cfg.loaded) return;
  if (_cfg.save()) {
    _dirty = false;
    toast("saved");
  }
}

void Dashboard::toast(const std::string& msg) {
  _toast = msg;
  _toast_until = Clock::now() + std::chrono::seconds(2);
}

int Dashboard::count_visible() const {
  int n = 0;
  for (const auto& p : _page)
    if (p.visible) ++n;
  return n;
}

std::string Dashboard::default_title(const std::string& id) const {
  const auto it = _default_titles.find(id);
  return it == _default_titles.end() ? id : it->second;
}

bool Dashboard::panel_visible(const std::string& id) const {
  const int i = page_index(id);
  return i >= 0 && _page[i].visible;
}

int Dashboard::page_index(const std::string& id) const {
  for (int i = 0; i < (int)_page.size(); ++i)
    if (_page[i].id == id) return i;
  return -1;
}

// ---------------------------------------------------------------------------
// --script
// ---------------------------------------------------------------------------

bool parse_script_token(const std::string& tok, ftxui::Event& out) {
  if (tok == "space")
    out = ftxui::Event::Character(' ');
  else if (tok == "enter" || tok == "return")
    out = ftxui::Event::Return;
  else if (tok == "esc" || tok == "escape")
    out = ftxui::Event::Escape;
  else if (tok == "tab")
    out = ftxui::Event::Tab;
  else if (tok == "shift-tab")
    out = ftxui::Event::TabReverse;
  else if (tok == "up")
    out = ftxui::Event::ArrowUp;
  else if (tok == "down")
    out = ftxui::Event::ArrowDown;
  else if (tok == "left")
    out = ftxui::Event::ArrowLeft;
  else if (tok == "right")
    out = ftxui::Event::ArrowRight;
  else if (tok == "pageup")
    out = ftxui::Event::PageUp;
  else if (tok == "pagedown")
    out = ftxui::Event::PageDown;
  else if (tok == "home")
    out = ftxui::Event::Home;
  else if (tok == "end")
    out = ftxui::Event::End;
  else if (tok == "backspace")
    out = ftxui::Event::Backspace;
  else if (tok.rfind("click:", 0) == 0) {
    // "click:X:Y": a left-button press at (X, Y).
    const std::string rest = tok.substr(6);
    const auto colon = rest.find(':');
    if (colon == std::string::npos) return false;
    const std::string xs = rest.substr(0, colon);
    const std::string ys = rest.substr(colon + 1);
    char* e1 = nullptr;
    char* e2 = nullptr;
    const long x = std::strtol(xs.c_str(), &e1, 10);
    const long y = std::strtol(ys.c_str(), &e2, 10);
    if (e1 == xs.c_str() || *e1 != '\0' || e2 == ys.c_str() || *e2 != '\0')
      return false;
    ftxui::Mouse m;
    m.x = static_cast<int>(x);
    m.y = static_cast<int>(y);
    m.button = ftxui::Mouse::Left;
    m.motion = ftxui::Mouse::Pressed;
    out = ftxui::Event::Mouse("", m);
    return true;
  } else if (tok.size() == 1)
    out = ftxui::Event::Character(tok[0]);
  else
    return false;
  return true;
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

// Phase 5: the active theme (CLI > config > first selectable) and the full
// selectable list (built-ins + custom files) for the `T` cycle. An unknown
// name is a warning + fallback, never an error.
static std::vector<std::pair<std::string, Theme>> resolve_themes(
    const UiOptions& opts, const Config& cfg, int& idx,
    std::vector<std::string>& warnings) {
  std::vector<std::pair<std::string, Theme>> themes = all_themes(&warnings);
  if (themes.empty())
    themes.push_back({"cyberpunk-blue", Theme::cyberpunk_blue()});
  const std::string want = !opts.theme.empty() ? opts.theme : cfg.theme;
  idx = 0;
  for (int i = 0; i < (int)themes.size(); ++i)
    if (themes[i].first == want) {
      idx = i;
      return themes;
    }
  warnings.push_back("theme '" + want + "' is unknown — using " +
                     themes[0].first);
  return themes;
}

int run_ui(Collector& collector, const Hardware& hw_in, const UiOptions& opts) {
  ColorMode mode;
  if (!resolve_color_mode(opts, mode)) return 2;

  const std::string path =
      opts.config_path.empty() ? Config::default_path() : opts.config_path;
  Config cfg = Config::load(path, opts.no_save);
  const Effective eff = effective_settings(opts, cfg);

  std::vector<std::string> twarn;
  int theme_idx = 0;
  auto themes = resolve_themes(opts, cfg, theme_idx, twarn);
  for (const auto& w : twarn)
    std::fprintf(stderr, "geth: theme: %s\n", w.c_str());

  FrameStats stats;
  auto component = ftxui::Make<Dashboard>(
      collector, hw_in, std::move(cfg), std::move(themes), theme_idx, mode,
      eff.interval_ms, opts.count, eff.window_idx, eff.columns, stats);
  if (!twarn.empty()) component->note_warning("theme: " + twarn.front());
  collector.set_interval_ms(eff.interval_ms);

  // A signal during the probe/setup phase: skip the UI and exit cleanly.
  if (util::g_stop.load()) return 0;

  ftxui::ScreenInteractive screen = ftxui::ScreenInteractive::Fullscreen();
  // SIGHUP is the one signal FTXUI does not handle; from here on (the termios
  // is still the user's normal one) it restores the terminal and exits.
  util::install_sighup_restore();

  // The small timer thread: posts Event::Custom at each tick and nothing
  // else. All sampling and rendering happen on the UI thread. It reads the
  // CURRENT interval every loop, so a `+`/`-` change takes effect on the
  // next tick.
  std::atomic<bool> quit{false};
  std::thread timer(
      [&screen, &quit, iv = &component->interval()] {
        while (!quit) {
          std::this_thread::sleep_for(
              std::chrono::milliseconds(iv->load()));
          if (quit) break;
          screen.PostEvent(ftxui::Event::Custom);
        }
      });

  // First frame at row 0 regardless of where the shell prompt sat (v1.0.1).
  // FTXUI enters the alternate screen inside Loop() and then draws frame 1
  // from the cursor's CURRENT row — it never homes the cursor, so with the
  // prompt low on the screen frame 1 started low and scrolled up. Enter the
  // alternate screen ourselves first: that saves the main screen and keeps
  // the cursor, then home + clear put the cursor at 0,0. FTXUI's own
  // re-entry inside Loop() is a no-op on a terminal already in the
  // alternate screen, so frame 1 is drawn at the top; on exit FTXUI's
  // `?1049l` restores the (saved, untouched) main screen.
  std::cout << "\x1b[?1049h"  // enter alternate screen (main screen saved)
            << "\x1b[H"        // cursor home
            << "\x1b[2J";      // clear the alternate screen
  std::cout.flush();

  screen.Loop(component);  // blocks until q / Esc / Ctrl-C (or count ticks)
  util::uninstall_sighup_restore();

  quit = true;
  timer.join();

  if (opts.stats) stats.print();
  return 0;
}

int run_render_frame(Collector& collector, const Hardware& hw_in, int W, int H,
                     unsigned long long warmup, const UiOptions& opts) {
  ColorMode mode;
  if (!resolve_color_mode(opts, mode)) return 2;
  if (W < 1 || H < 1) {
    std::fprintf(stderr, "geth: --render-frame size must be at least 1x1\n");
    return 2;
  }

  const std::string path =
      opts.config_path.empty() ? Config::default_path() : opts.config_path;
  Config cfg = Config::load(path, opts.no_save);
  const Effective eff = effective_settings(opts, cfg);

  std::vector<std::string> twarn;
  int theme_idx = 0;
  auto themes = resolve_themes(opts, cfg, theme_idx, twarn);
  for (const auto& w : twarn)
    std::fprintf(stderr, "geth: theme: %s\n", w.c_str());

  // Collect warmup samples. Demo mode sleeps nowhere: instead each sample is
  // stamped on a VIRTUAL clock spaced by the effective interval, so 60
  // samples fill the whole 60 s graph window (back-to-back wall-time samples
  // would all land within one millisecond and every graph column would show
  // the same value; the demo source follows out.time, so its waves advance
  // too). Real hardware sleeps the real interval.
  if (opts.demo) {
    TimePoint vnow = collector.latest().time;  // the probe's warm-up sample
    for (unsigned long long i = 0; i < warmup; ++i) {
      vnow += std::chrono::milliseconds(eff.interval_ms);
      collector.set_sample_time(vnow);
      collector.sample();
    }
  } else {
    for (unsigned long long i = 0; i < warmup; ++i) {
      collector.sample();
      if (i + 1 < warmup)
        std::this_thread::sleep_for(std::chrono::milliseconds(eff.interval_ms));
    }
  }

  FrameStats stats;
  ftxui::Screen screen(W, H);
  screen.Clear();
  auto dash = ftxui::Make<Dashboard>(collector, hw_in, std::move(cfg),
                                     std::move(themes), theme_idx, mode,
                                     eff.interval_ms, /*count=*/0,
                                     eff.window_idx, eff.columns, stats);
  if (!twarn.empty()) dash->note_warning("theme: " + twarn.front());
  collector.set_interval_ms(eff.interval_ms);
  dash->set_size(W, H);

  // --script: replay synthetic events against the live Dashboard, then render
  // the result. The final state is what gets printed.
  if (!opts.script.empty()) {
    std::size_t start = 0;
    for (std::size_t i = 0; i <= opts.script.size(); ++i) {
      if (i != opts.script.size() && opts.script[i] != ',') continue;
      const std::string tok = opts.script.substr(start, i - start);
      start = i + 1;
      if (tok.empty()) continue;
      ftxui::Event ev;
      if (!parse_script_token(tok, ev)) {
        std::fprintf(stderr, "geth: --script: unknown token '%s'\n",
                     tok.c_str());
        return 2;
      }
      dash->OnEvent(ev);
    }
  }

  const auto t0 = Clock::now();
  const long long a0 = alloc_count();
  const ftxui::Element el = dash->OnRender();
  ftxui::Render(screen, el);
  stats.note_render(
      std::chrono::duration<double, std::micro>(Clock::now() - t0).count(),
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
