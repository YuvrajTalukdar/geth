# GETH — Configuration

The dashboard reads `$XDG_CONFIG_HOME/geth/config.toml` (or
`~/.config/geth/config.toml` when `XDG_CONFIG_HOME` is unset). On the first
run there is no file: the defaults are used, and the file is written only the
first time a setting actually changes (never on a bare launch).
`--config PATH` points at a different file, and `--no-save` makes it
read-only (changes apply for the session only, and no "saved" toast appears).
A config the app wrote is a good starting point to edit by hand.

The uninstall paths (`pacman -R geth`, `cmake --build build --target
uninstall`) never touch this file. Removing `~/.config/geth` (or just the
`config.toml`) restores the defaults.

## Schema

Unknown keys (and malformed values) are warned about on `stderr` and ignored,
never an error:

```toml
[general]
interval_ms = 1000      # 250 | 500 | 1000 | 2000 | 5000
window = "1m"           # "30s" | "1m" | "2m" | "5m" | "10m"
columns = "auto"        # "auto" | 1..8
title = ""              # header title override ("" = auto: CPU & GPU names)
theme = "cyberpunk-blue"# built-in or custom theme name (see --list-themes)
background = "transparent"  # "transparent" | "solid" (paints the theme bg)
graph_renderer = "auto"     # "auto" | "pixels" | "text" | "steps" (auto = kitty when available)

# One [[panels]] table per panel, in display order.
[[panels]]
id = "cpu"              # stable panel id (below)
visible = true
title = ""              # "" = the default title
hidden_series = []      # series ids hidden in this panel

# Per-GPU overrides, keyed by PCI bus id. A `name` here is a GLOBAL rename:
# it applies to every panel, legend, and the header for that bus.
[gpus."0000:01:00.0"]
name = "RTX 3050"
integrated = "auto"     # "auto" | true | false

[logging]
dir = ""                # CSV output directory ("" = $XDG_DATA_HOME/geth/logs)
autostart = false       # start CSV logging at launch (overridden by --log)
```

The retired v1.0.3 key `graph_style` (the graph line style, `"braille" |
"blocks"`) is still accepted so old configs keep loading: it is ignored in
favor of `graph_renderer`, a one-time warning is shown, and it is dropped
the next time the config file is saved.

`auto` resolves to the **optimized** renderer: Kitty when the terminal
speaks it (run geth directly in Konsole, kitty, or Ghostty — no tmux), else
text. The **Sixel** path — the only image option inside tmux — is
**EXPERIMENTAL**: a sixel image does not persist, so geth re-emits every
panel every frame (higher GPU load, no re-send policy). It is not the `auto`
default; select it with `graph_renderer = "pixels"` (or `--renderer pixels`),
and geth flags it as experimental when it is active.

`steps` is a third graph mode, an nvtop-style step-line chart: the history
is drawn as box-drawing characters — one data point per character column,
newest at the right edge, flat runs as `─` and value changes as square-
cornered vertical steps (no fill, no braille). The graph width spans the
whole time window; each column is the mean of the samples in that slice.
Steps are always text-rendered (box-drawing glyphs), so they work in any
terminal and in tmux, foreground-only (transparent background). They show
history, not a snapshot. The `g` key cycles text → pixels → steps → text.

The v1.2.0 `graph_renderer = "bars"` value is retired: it is still accepted
(so old configs keep loading), is migrated to `"steps"`, a one-time warning
is shown, and it is written as `"steps"` the next time the config file is
saved.

### Panel and series ids

Panel ids: `cpu`, `mem`, `temps`, `power`, `net`, `disk`, and
`gpu.<bus>.usage` / `gpu.<bus>.vram` (one usage + one VRAM panel per GPU).
Series ids look like `cpu.usage`, `mem.ram_used`, `gpu.<bus>.vram`.

A per-panel `title` overrides the default for that one panel only; the
`[gpus."…"].name` rename is what changes *every* occurrence of the GPU's name
(both its panels, the temps/power legends, and the header).

### Merge rules on load

- A panel whose hardware is **absent** stays in the file but is not shown (it
  appears as "(not present)" in the panel manager, so its settings are kept
  for the day the hardware returns).
- A panel whose hardware is **new** is inserted **visible** at its default
  position.
- A panel present in **both** keeps the file's order and settings.

### Save behavior

- Saves are **atomic** (a temp file in the same directory, then `rename`), so
  a crash never leaves a half-written config, and a stale `.tmp` from a
  crashed save is dropped on the next load.
- A file that does not parse is **never overwritten**: the defaults are used,
  the footer shows the parse error, and `save()` refuses.
- Test-safety: when `GETH_TEST=1` is set (soaks, CI, dev harnesses), saving
  is refused anywhere under the real `~/.config`. Set `XDG_CONFIG_HOME` to a
  temporary directory instead.

### CSV logging

`[logging]` controls the opt-in CSV data logger (the `l` key; see
[Logging to CSV](../README.md#logging-to-csv) in the README). It records only
what you have chosen to view — the visible panels in display order, each shown
series and numeric info row, de-duplicated by metric.

- `dir` — where the CSV files are written. `""` (the default) means
  `$XDG_DATA_HOME/geth/logs/`, falling back to `~/.local/share/geth/logs/`.
  Overridden per run by `--log-dir`.
- `autostart` — when `true`, logging starts as soon as the UI comes up.
  `--log` forces it on for one run; either way the `l` key toggles it at
  runtime.

Files are named `geth-YYYYMMDD-HHMMSS.csv` and are never overwritten. Changing
the visible selection while recording (hiding a panel, toggling a series,
renaming or reordering panels) starts a new file; changing theme, graph mode,
window, columns, or the sampling interval does not.
