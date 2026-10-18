// GETH configuration file (Phase 4, Part B).
//
// Location: $XDG_CONFIG_HOME/geth/config.toml, falling back to
// ~/.config/geth/config.toml. --config PATH overrides the location,
// --no-save makes the file read-only. The first run has no file: the
// defaults are used and the file is written ONLY once something actually
// changes (never on first run).
//
// Schema:
//   [general]
//   interval_ms = 1000        # 250 | 500 | 1000 | 2000 | 5000
//   window = "1m"             # "30s" | "1m" | "2m" | "5m" | "10m"
//   columns = "auto"          # "auto" | 1..8
//   title = ""                # header title override ("" = the auto title)
//   theme = "cyberpunk-blue"  # a built-in or a custom themes/<name>.toml
//   background = "transparent"  # "transparent" | "solid" (paints the theme's bg)
//   graph_renderer = "auto"   # "auto" | "pixels" | "text" (how graphs are drawn)
//
//   [[panels]]                # one table per panel, in display order
//   id = "cpu"
//   visible = true
//   title = "CPU"
//   hidden_series = []        # series ids hidden in this panel
//
//   [gpus."0000:01:00.0"]     # per-GPU overrides, keyed by PCI bus id
//   name = "RTX 3050 Laptop"
//   integrated = "auto"       # "auto" | true | false
//
// Merge rules on load:
//   - a panel whose hardware is absent is kept in the file, not shown;
//   - a panel whose hardware is new is inserted VISIBLE at its default
//     position (build_default_page's slot);
//   - unknown keys (and malformed values) are WARNED about and ignored —
//     they are never an error.
// A file that does not parse: the defaults are used, the footer shows
// "config error (line N): … — using defaults, not saving", and the file is
// NEVER overwritten (save() refuses while loaded == false).
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "ui/panel_spec.hpp"

namespace geth::ui {

// One [[panels]] entry, in file order.
struct PanelEntry {
  std::string id;
  bool visible = true;
  std::string title;  // "" = the default title
  std::vector<std::string> hidden_series;  // SeriesIds hidden in this panel
};

// One [gpus."<bus>"] entry.
struct GpuEntry {
  std::string name;  // "" = the detected name
  std::optional<bool> integrated;  // nullopt = "auto"
};

struct Config {
  // [general]
  long long interval_ms = 1000;
  int64_t window_ns = 60LL * 1000000000LL;
  int columns = -1;  // -1 = auto, else 1..8
  std::string title;  // "" = the auto header title
  // Phase 5: theme name (built-in or a custom themes/<name>.toml); validity
  // is checked at resolution time, not here.
  std::string theme = "cyberpunk-blue";
  // "transparent" (default: no background color is ever painted) or "solid"
  // (the theme's bg color is painted across the whole screen — the only
  // sanctioned background path).
  std::string background = "transparent";
  // How the graph interiors are drawn: "auto" (pixels when the terminal
  // speaks a graphics protocol, else text — the default), "pixels" (force
  // the image protocol; falls back to text with a warning), or "text".
  // Toggled live with `g` between the available ones.
  std::string graph_renderer = "auto";
  // The file contained the retired graph_style key (v1.0.3): it is accepted,
  // ignored, and dropped on the next save; the UI warns once.
  bool had_graph_style = false;

  std::vector<PanelEntry> panels;  // file order
  std::map<std::string, GpuEntry> gpus;  // PCI bus id -> override

  // Environment -----------------------------------------------------------
  std::string path;      // resolved file path
  bool no_save = false;  // --no-save: read-only
  bool exists = false;   // the file existed at load time
  bool loaded = true;    // false => unparseable; the defaults are in force
  std::string error;     // "line N: message" (the footer warning)
  std::vector<std::string> warnings;  // unknown keys etc. (stderr, at load)

  // $XDG_CONFIG_HOME/geth/config.toml, or ~/.config/geth/config.toml when
  // XDG_CONFIG_HOME is unset/empty.
  static std::string default_path();
  // Loads `path`; missing file = first run = defaults (exists = false).
  // Never throws: an unparseable file leaves loaded = false + error set.
  static Config load(const std::string& path, bool no_save);
  // Serializes and writes ATOMICALLY (tmp file in the same directory +
  // rename). No-op (true) under --no-save; refuses (false) when the loaded
  // file was unparseable — a broken config is never overwritten.
  bool save() const;
  // The file contents serialize() would write (also used by the tests).
  std::string serialize() const;

  // Rebuilds panels from the live page + the manager order: the PRESENT
  // panels (visibility, titles, hidden series) in their `order` position,
  // and the entries whose hardware is absent kept with their saved settings.
  // Call before every save.
  void sync_panels(const std::vector<PanelSpec>& page,
                   const std::vector<std::string>& order);
};

// The allowed [general] values (anything else: warn + default).
bool valid_interval_ms(long long ms);  // 250 | 500 | 1000 | 2000 | 5000
bool valid_window_ns(int64_t ns);      // 30 s | 1 m | 2 m | 5 m | 10 m
bool valid_columns(int columns);       // -1 (auto) or 1..8

// The window's config string ("30s", "1m", ...) or "" when not a preset.
std::string window_to_string(int64_t ns);
// The reverse of window_to_string; false on anything else.
bool window_from_string(const std::string& s, int64_t& out);

// Merges `cfg` into the default page for the current hardware:
//   - panels present in both keep the FILE's order and settings;
//   - new hardware is inserted visible at its default position (its
//     build_default_page slot relative to the known panels);
//   - absent-hardware entries are dropped from the page (they remain in the
//     file via Config::panels).
std::vector<PanelSpec> merge_page(const std::vector<PanelSpec>& default_page,
                                  const Config& cfg);
// Applies the [gpus."..."] overrides (name / integrated) to a Hardware copy.
void apply_gpu_overrides(Hardware& hw, const Config& cfg);

}  // namespace geth::ui
