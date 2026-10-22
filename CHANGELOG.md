# Changelog

All notable changes to GETH. The format follows the five-phase build plan.

## 1.5.1 — CSV column names that make sense outside the panel (2026-10-22)

Every loggable metric now carries an **explicit column name** of the form
`<device> <quantity> [unit]`, defined next to the series/info-row spec (a
`log_name` field) and built from the device's *current display name* — never
from a legend label or a panel title. So a column's name is stable no matter
which other panels are visible (the old panel-title prefixing on collision is
removed), a GPU rename flows through to exactly that GPU's columns, and two
GPUs that end up with the same display name get ` #2` (then ` #3`, …) on the
later one's name. Examples: `CPU Usage [%]`, `CPU Package Power (APU PPT) [W]`
(`<source>` ∈ `RAPL` / `amd_energy` / `zenpower` / `APU PPT`),
`RTX 3050 Laptop Core Clock [MHz]`, `RAM Used [B]`, `Network Download [B/s]`,
`Disk Write [B/s]`. The full set is listed in the README's logging section.

The dev pty harnesses are corrected too: they used to `break` on the pty EIO
(child closed its end) *before* reaping the zombie, losing the real exit
status and reporting a bogus "signal 119" (`-9` misread). They now reap
properly and report the real status, and rely on `--count` for a clean exit
instead of SIGTERM/SIGKILL timeouts. A new `tools/count_exit_test.py` drives
`geth --demo --count N --log` in a pty and asserts it exits spontaneously
(code 0) with a complete CSV.

## 1.5.0 — CSV logging: record exactly what you view (2026-10-22)

A new opt-in CSV data logger, toggled with **`l`** (or `--log` /
`--log-dir` on the command line, or `[logging]` in the config). It records
**only what you have chosen to view** — every visible panel in display order,
each shown series and numeric info row, de-duplicated by metric so a value
shown in two panels gets one column. While recording, the header shows a
`● REC hh:mm:ss` indicator, and one row is written per sampling tick (rows
keep coming while the display is paused).

- **Columns** — `timestamp` (local ISO 8601 with ms and UTC offset),
  `elapsed_s` (3 decimals, since this file started), then one column per
  logged value. Header names are the names currently shown (including GPU
  renames); on a collision the panel title is prefixed, then a numeric suffix
  as a last resort. Precision per unit: `%` / `°C` one decimal, `W` two,
  `MHz` / `B` / `B/s` integer.
- **Format** — RFC 4180, UTF-8 (no BOM), `\n` line endings, `.` as the decimal
  point regardless of locale. A missing value (a sleeping GPU, a rate's first
  tick, a suspend gap) is an **empty** field, never 0. Fields containing
  commas, quotes or newlines are quoted, with quotes doubled.
- **Files** — `geth-YYYYMMDD-HHMMSS.csv` in `$XDG_DATA_HOME/geth/logs/`
  (fallback `~/.local/share/geth/logs/`), never overwritten. Changing the
  visible selection while recording (hiding a panel, toggling a series,
  renaming or reordering) starts a new file (`…-2.csv`, `…-3.csv`, …);
  changing theme, graph mode, window, columns, or the sampling interval does
  not.
- **Durability** — every row is written unbuffered (one `write()` per row), so
  a crash, `Ctrl-C`, or the SIGHUP terminal-restore path loses at most the row
  in flight; the file is always complete and parseable.
- **Config** — a new `[logging]` table: `dir` (destination) and `autostart`
  (start at launch; `--log` forces it on for a run).
- **Performance** — zero heap allocations per row after warm-up (a reused row
  buffer, manual `snprintf` formatting, no `%f`), and the column fingerprint is
  recomputed only when the visible selection actually changes. A counting
  allocator test proves the steady state is allocation-free.
- **Tests** — column selection (default page, hide panel/series, de-dup, order,
  GPU renames), exact values and precision, the de_DE locale, quoting,
  rotation, start/stop sessions, unwritable-directory errors, the GETH_TEST
  guard, and the zero-allocation steady state; plus dashboard-level
  start/stop/rotate and the REC indicator (224 cases / 526,321 assertions;
  Debug ASan/UBSan and Release both green).
- **Docs** — a README "Logging to CSV" section, docs/CONFIG.md (`[logging]`),
  the geth.1 man page (`-log`, `-log-dir`, the `l` key, FILES, ENVIRONMENT),
  and the in-app help overlay and footer.

## 1.4.0 — blaze-orange: new built-in theme (2026-10-20)

A fifth built-in theme, **blaze-orange** — a bright, saturated orange,
distinctly redder and punchier than `amber` (whose `#FFB000` is a yellow-gold).
Select it with `[general] theme = "blaze-orange"`, `--theme blaze-orange`,
`--list-themes`, or the `T` cycle, which now runs
cyberpunk-blue → matrix-green → crimson-red → amber → blaze-orange → back.

- **Series palette** (role slots): orange `#FF7A1A`, cyan `#4DD9FF`,
  yellow `#FFF36B`, periwinkle `#9D8CFF`; slots 5–6 are the standard
  40 % toward-white mixes (`#FFAF76`, `#94E8FF`). The palette passes every
  Part C rule with margin (tightest: ΔE 20.6 normal / 14.3 deutan between
  the cyan and periwinkle slots; smallest contrast 7.0:1 vs `#0B0F14`) —
  the full measured matrix is printed by the test suite.
- **Tests:** the spec-table, 16-color-mapping, Part C validation,
  resolve/all_themes, render, and no-background suites now cover all five
  built-ins (207 cases / 526,046 assertions, both Debug and Release).
- **Docs:** docs/THEMES.md (theme table + measured margins), the README
  theme list and gallery, and the built-in names in the geth.1 `-theme`
  entry.

## 1.3.0 — steps mode: nvtop-style step-line chart (2026-10-20)

The v1.2.0 bars mode is replaced by a new third graph mode, **steps** — an
nvtop-style step-line history chart, cycled with `g` and selectable via
`graph_renderer = "steps"` / `--renderer steps`. The history is drawn with
box-drawing characters: one data point per character column, newest at the
right edge, flat runs as `─`, and a value change as a square-cornered
vertical step (`┘`/`┌` stepping up, `┐`/`└` stepping down, `│` between) —
no fill, no braille. The graph width spans the whole time window (`t`), each
column is the mean of the samples in that slice, and a slice with no sample
is interpolated. Multi-series panels draw each series in its own theme color,
with junction glyphs (`┼`/`┴`/`┬`) where one line crosses another's vertical
step, in the color of the series that owns the vertical. Steps are always
text-rendered (foreground-only, transparent background), so they work in any
terminal and in tmux, and they share the line charts' axes, tick rows, and
auto-scaling.

- **The `bars` value is retired.** `graph_renderer = "bars"` (v1.2.0) is
  still accepted so old configs keep loading: it is migrated to `"steps"`, a
  one-time toast is shown, and the file is rewritten with `"steps"`.
  `--renderer bars` is rejected with a message pointing at `"steps"`.
- **`g` now cycles text → pixels → steps → text** (saved to the config);
  with no image protocol it cycles text ↔ steps.
- **Tests:** exact glyph/position assertions for the step-line renderer
  (constant line, step up/down, first column, gaps, multi-series junctions,
  gridlines), plus the config round-trip and the `bars` → `steps` migration.
- **Docs:** README (a mode table with one screenshot per mode),
  docs/CONFIG.md, the geth.1 man page, and an nvtop acknowledgement (the step
  line is inspired by nvtop, GPLv3; reimplemented independently for GETH — no
  nvtop code is copied).

## 1.2.0 — bars mode: nvtop-style per-unit meters (2026-10-20)

A third graph mode, cycled by `g` and selectable via
`graph_renderer = "bars"` / `--renderer bars`. Instead of a scrolling history
line chart, each panel draws one vertical bar per unit — a bar per CPU core,
per GPU, per sensor, per network interface, and per disk device — filled to
the current value and ramped green → yellow → red as it rises. Bars are always
text-rendered (block columns), so they work in any terminal and in tmux, and
they show the CURRENT value (a snapshot), not history. The y axis matches each
bar's scale (100 % for usage, the total for memory, 110 °C for temps,
128 W for power, 100 MiB/s for net, 1 GiB/s for disk). `g` now cycles
text → pixels → bars → text (saved to the config); with no image protocol it
cycles text ↔ bars.

## 1.1.1 — Sixel is experimental; iGPU and RAPL fixes (2026-10-20)

Follow-up to 1.1.0: the pixel renderer now works inside tmux (via Sixel),
but that path is **experimental**; the direct-terminal Kitty path is the
**optimized** one. Also fixes the iGPU load of the pixel renderer and a RAPL
triple-count on AMD.

### Sixel (tmux) is experimental; Kitty is the optimized path

The iGPU re-send policy (quiet panels are not re-uploaded) applies to the
Kitty renderer only: a Kitty image persists in the terminal and is replaced
in place. A sixel image (the tmux+Konsole path) does not persist, so geth
must re-emit every panel every frame — higher GPU load, no re-send savings.

- **Sixel is now EXPERIMENTAL and opt-in.** `graph_renderer = "auto"`
  resolves to the optimized renderer: Kitty when the terminal speaks it
  (run geth directly in Konsole / kitty / Ghostty — no tmux), else text. It
  no longer selects Sixel. Sixel is used only with an explicit
  `graph_renderer = "pixels"` (or `--renderer pixels`) in a tmux pane whose
  outer terminal supports Sixel.
- **A one-time notice** flags Sixel as experimental when it is active
  (at startup, or when `g` cycles into it), pointing at the optimized
  path. `--probe-terminal` reports the renderer the rules would actually
  choose, so in a tmux pane `auto` now reports `renderer: text` (with a
  note that Sixel is available as an experimental opt-in).
- Docs (README, CONFIG, the man page) label Sixel-in-tmux experimental and
  recommend direct Konsole (Kitty) as the optimized renderer.

### Pixels (Sixel) inside tmux

The v1.1.0 pixel renderer never reached users behind tmux. Two
geth-side problems, both now fixed (verified end-to-end: real geth in a
tmux 3.8 pane against a sixel-advertising outer terminal):

- **The probe now runs inside the multiplexer.** tmux ≥ 3.8 (built with
  sixel) advertises Sixel in its DA1 reply and re-encodes every stored
  sixel image to the attached terminal — so probing inside the pane is
  correct and useful. `--probe-terminal` in a tmux pane reports
  `inside multiplexer: yes (tmux/screen)` and `renderer: pixels (sixel)`
  when the outer terminal supports Sixel. (screen still does not
  advertise Sixel, so it stays text there.)
- **The Sixel encoder now emits the classic DEC dialect** that tmux's
  parser (image-sixel.c) and Konsole's own sixel handler both accept:
  `ESC P 9;1q` (no space — a space is an intermediate byte and both
  parsers drop the entire DCS), the mandatory `"1;1;W;H` size,
  `#c;2;r;g;b` register declarations, and data bytes `0x3f + mask`
  (arithmetic: both parsers decode with `byte - 0x3f`). The previous
  named-param form was rejected silently by tmux's parser, which drops
  the whole image on any non-classic byte. Sixel pixels are opaque
  (Konsole's indexed-8 has no alpha): semi-transparent pixels composite
  over the theme's bg, which is also register 0 — the seam is invisible
  when the theme bg matches the terminal background.

End-to-end: 149/149 panel images crossed the tmux boundary as
re-encoded `9;1q` sixel; with a non-sixel outer terminal the same 149
fall back to tmux's text rendering, as expected.

### iGPU load: data-space re-send policy

The pixel renderer re-sent every panel's chart image every tick, which
measured 30-50% iGPU on an APU (vs 2-8% for text). Re-sends are now gated on
the DATA, not the raster: a panel is re-uploaded only when a real change
moves more than a threshold fraction of its columns, or its y-scale / gap
count / rect changed, or a 10 s cap elapsed. Quiet panels are skipped — the
placed image persists (Kitty) so the picture stays — cutting idle image
traffic ~3.5-4x while real movement still appears within one tick. The gate
is Kitty-only (Sixel must re-emit every frame; see below).

### RAPL: count each package domain once

On some AMD machines the single RAPL package counter was reachable through
three different sysfs paths, so CPU package power was triple-counted. Probe
domains are now deduplicated by canonical path, so the counter is read once.

### Sixel re-emits every frame (regression fix)

A sixel image does not persist in the tmux path: tmux attaches it to the
screen cells it covers and FTXUI's next full-screen rewrite detaches it. The
quiet-skip (above) therefore made quiet panels' graphs vanish in tmux. The
skip is now Kitty-only; Sixel re-emits every panel every frame so its graphs
stay on screen (at the cost of the higher GPU load — hence experimental).

## 1.1.0 — Real charts: pixel renderer + better text fallback (2026-10-18)

### New: pixel renderer (Kitty graphics protocol / Sixel)

When the terminal speaks the Kitty graphics protocol or Sixel, every panel
graph is now a pixel image: smooth anti-aliased lines, a soft translucent
fill, faint gridlines, on a transparent background — the KDE System
Monitor look. The dashboard probes the terminal on launch
(Kitty query + DA1 + a test transfer, `src/ui/term_probe.cpp`), rasters
each graph rect (`src/ui/graph_image.cpp`), and injects the images once
per tick through the Kitty APC (`a=T`, zlib `o=z`, in-place replace by
image id) or the Sixel encoder (`src/ui/graph_backends.cpp`). Images are
deleted on every exit path: normal quit, SIGTERM (FTXUI's clean-exit
path), and SIGHUP (a double-buffered delete stream is written ahead of
the existing terminal restore, so a hung-up terminal never shows orphan
images). Under tmux/screen the app always uses text (multiplexers strip
the escapes).

- `g` cycles the renderer (pixels / text) and saves it.
- `graph_renderer = "auto" | "pixels" | "text"` in the config
  (`auto` = pixels when a protocol is detected, else text); the retired
  `graph_style` key is still accepted, warned once, and dropped on the
  next save.
- `--renderer pixels|text` forces the renderer for one run (not saved).
- `--probe-terminal` prints the detected protocols, the cell pixel size,
  and the chosen renderer.

Verified against Konsole's source (`processGraphicsToken`): it renders
Kitty `a=T` transfers pixel-exact above the text and ignores `a=r`
entirely, so the backend uses `a=T` — the diagnostic `a=r` matrix from
early probing had read that as "no support".

### Redesigned text fallback

- Single-series panels: a solid **area chart** (eighth blocks with a
  per-column vertical gradient) instead of a braille line.
- Multi-series panels: one-dot braille lines, no fill (the old dotted
  fill under multiple lines is gone), primary drawn last.
- The `blocks` style is removed (its config value migrates as above).

### New: `--render-png`

`--render-png COLSxROWS` composites one frame — the generated background
(gradient + soft radial glows, or a flat fill for
`background = "solid"`), the text glyphs from an embedded DejaVu Sans
Mono coverage set, and, in pixels mode, the graph rasters — into an
8-bit RGB PNG on stdout (own IHDR/IDAT/IEND writer over zlib). The glyph
set is generated by `tools/gen_glyphs.py`; the braille block is drawn as
procedural square dots (geometry measured from DejaVu Sans) because
DejaVu Sans Mono ships it as tofu boxes.

### Performance

The rasterizer's canvas now accumulates premultiplied color (all
compositing divides by the constant 255 instead of the per-pixel output
alpha) and the capsule AA takes its single sqrt only on the 1-px edge
band. `tools/measure_pixel_perf.py` measures render cost in a pty
against a fake terminal (wall, CPU % via rusage, bytes/s). Release,
`--demo`, 1 Hz, 30 ticks: 250x70 pixels 3.7 % CPU / 741 KB/s, 250x70
text 1.7 % / 55 KB/s, 125x35 pixels 1.2 % — within the 2 % budget at
common sizes; at 250x70 the pixel renderer is ~1.8x over, dominated by
zlib on the 10 MB/tick RGBA (already at the fastest meaningful level)
and the full-frame text rewrite shared with text mode.

### Tests

182 cases / 525,602 assertions, green on Debug (ASan+UBSan) and Release.
New: the rasterizer (line rows, AA partial alpha, fill gradient, gridline
alignment, NaN gaps), the Kitty encoder (exact APC structure, chunking,
delete commands, reassemble-and-inflate round trip), the Sixel encoder
(palette cap, `P2=1`, R6 run decode, height padding), and the terminal
probe (canned replies: Kitty, Sixel-only, no reply, tmux).

### Screenshots

The README gallery was recaptured live in Konsole with real hardware
data: theme and layout shots in the pixel renderer (Kitty graphics
protocol), panel manager and help in the text renderer (the overlays
replace the charts with text lists). `style-blocks.png` is removed.

## 1.0.3 — CPU power fix, line visibility, theme palettes (2026-10-18)

### Fix: CPU package power frozen on APU machines

CPU package power stayed frozen at its first value on machines where the
package counter comes from the APU's PPT (RAPL unreadable without a fix).
Root cause: the `Collector` reuses one `Snapshot` between ticks, and
`CpuPowerSource` wrote the power fields only when a backend produced a
value — so the APU-PPT lift's "a counter source already won" guard read the
stale value the lift itself had stored on the previous tick, and never ran
again. Fix: `Collector::sample()` resets `package_power_w` /
`package_power_source` at the top of every tick. New `--trace <id>` flag
prints one series' raw read, parsed value, lift decision, and stored value
per tick (`--trace cpu.power`).

### Line visibility

- Graph lines are now **two dots tall** in braille (the plotted dot plus the
  dot above it), so lines stay visible at steep slopes and with dense series.
- Fills are **count-based**: a one-series panel is filled, a two-series panel
  fills only the primary series, three or more series have no fill. Fills
  sit under the lines; the primary series is drawn last so it is never
  covered.
- **New `blocks` graph style** — 2×2 quadrant glyphs instead of braille,
  selected with `[general] graph_style = "braille" | "blocks"` or the `g` key
  (toggles live, saved to the config).
- **Distinct colors within a panel**: series colors are assigned by device
  role (CPU = slot 1, first discrete GPU = 2, iGPU = 3, second discrete GPU
  = 4; extra GPUs and a second series of one device take the next free
  slot), so two series that share a panel are always distinguishable.
  `docs/THEMES.md` documents the slots.

### Theme palettes

- The four built-in palettes are now role-based six-slot palettes with slots
  5–6 as slots 1–2 mixed 40% toward white.
- **Validation** (test suite for the built-ins; at load for custom themes —
  a warning, never an error): pairs among slots 1–4 need ΔE ≥ 15 (OKLab,
  normal vision) and ΔE ≥ 8 under *both* protanopia and deuteranopia
  simulation (Machado 2009 matrices on linear sRGB); every slot needs ≥ 3:1
  WCAG contrast against `#0B0F14`. One sanctioned exception: matrix-green's
  slots 1↔4 measure 7.9 (protan) / 7.1 (deutan) — allowed at ≥ 7.0 for both
  simulations because every series carries a labelled legend.

### Tests

163 cases / 509,316 assertions, green on Debug (ASan+UBSan) and Release.
New: four collector regression tests for the power lift (including a
RAPL→PPT source switch on the same tick), the 16-glyph blocks table
(verified against Unicode names), graph-style parsing, blocks rendering,
the count-based fill rule, and the palette validation (all four built-ins,
a rejected bad palette, and the matrix-green exception under a different
name).

## 1.0.2 — Layout: always use the whole window (2026-10-17)

The dashboard could leave blank rows and hide panels without telling
the user in short terminals (repro: `geth --demo` at 210×19 scrolled
and left up to 5 blank rows above the footer). Panels now tile the
content area exactly — no blank rows or columns, at every size and in
every mode, including while scrolling.

### New: the Tight panel mode and abbreviated axis labels

- **Tight**: graph ≥ 2 rows, top/bottom axis labels only, abbreviated
  labels. Values stay in the border title. Minimum height 4 (border 2
  + graph 2).
- **Compact** no longer spends a row on a one-line legend by default
  (the values appear once, in the border title); a legend line is
  drawn only when the border title truncated with "…" and a spare row
  exists. Minimum height 5.
- **Abbreviated axis labels** (`100%`, `100°`, `24G`, `1.5T`, `20W`,
  `1.5kW`, `768K/s`, `1.5M/s`) whenever a panel's inner width is
  below 60 columns, in any mode. The narrower axis also lowers the
  minimum panel width (e.g. the network panel: 42 → 34 columns), so
  more columns fit.

### Layout selection

- For every candidate column count the engine gives each panel the
  best mode that fits (Full → Compact → Tight) and splits the content
  height equally among the rows (leftover to the top rows). A
  candidate is feasible when every panel fits at least Tight.
- Ranking: an all-Full layout always beats a degraded one; within a
  tier the candidates rank by the largest smallest graph height, then
  the largest total graph area, then the fewest degraded (non-Full)
  panels. Ties keep the fewer columns.
- Scrolling only starts when no column count fits every panel at
  Tight.

### Scrolling fills the screen

- The scroll window shows `k = content_height / 4` consecutive grid
  rows, stretched to fill the viewport exactly at every offset — no
  blank rows, no snapping to fixed row boundaries.
- The footer count is now the number of hidden rows (`total − k`,
  constant while scrolling) with conditional arrows: `3 more ▼` at
  the top, `▲ 3 more ▼` in the middle, `▲ 3 more` at the bottom.
- The scroll tail can never be truncated: on narrow terminals the key
  hints are the part that gets cut, so the indicator is always
  visible.
- Mouse-click panel focus in scroll mode follows the same window
  model as the renderer.

### Tests

- New: a 1,230-size sweep (widths 40–320 step 7 × heights 12–100
  step 3) over 10/8/6-panel pages asserting the tiling invariant
  (no gaps, no overlaps, exact coverage); scroll-indicator tests
  (top/middle/bottom, and the truncation fix at 44 columns); a
  resize-sequence test (300×80 → 210×20 → 120×40 → 80×24 → 160×48)
  asserting the re-layout at every step. 152 cases / 508,882
  assertions, green on Debug and Release.
- The 160×48 and 250×70 non-regression sizes stay all-Full with no
  scrolling (their axes now use the abbreviated form, as any panel
  narrower than 60 inner columns does).

### Screenshots

- All nine `docs/screenshots/` images recaptured live from a Konsole
  window running `geth --demo` (Spectacle, 1920×1080 screen): themes,
  panel manager, and help at 205×58 (windowed, 11 pt), the wide layout
  at 236×71 (fullscreen, 10 pt), the narrow layout at 111×31, and the
  compact layout at 85×25. The abbreviated axes are visible in every
  panel narrower than 60 inner columns; at 85×25 all ten panels now
  fit in two columns with no scrolling (previously one column hid
  seven).

## 1.0.1 — Release polish (2026-10-16)

Distribution-ready: first-frame positioning fix, install layout, Arch
package, screenshots, and reference documentation.

### Fixes

- **First-frame position**: after entering the alternate screen, the
  cursor is homed and the screen cleared before the first frame is
  drawn, so the dashboard starts at row 0 even when the shell prompt
  sits near the bottom of the terminal. Previously a full-height frame
  drawn from the prompt row scrolled the page up (verified in a pty
  harness with the prompt placed 30 rows down: 0 scroll-ups now, 30
  before).

### License and version

- **MIT license** (`LICENSE`, copyright Yuvraj Talukdar). The SPDX
  identifier lives in a single CMake variable (`GETH_LICENSE`); the
  version is defined once in `CMakeLists.txt`. The PKGBUILD's `pkgver`
  and `license=` are checked against them when the package is built —
  a mismatch fails the build.

### Install layout and packaging

- `cmake --install` places: `bin/geth`,
  `share/geth/themes/example.toml`,
  `share/geth/99-geth-rapl.rules` (shipped, **not** activated),
  `share/man/man1/geth.1.gz`, `share/doc/geth/{README,CHANGELOG}.md`,
  `share/licenses/geth/LICENSE`. It never touches `/etc` or
  `~/.config/geth`.
- `cmake --build <build> --target uninstall` removes exactly the files
  the install wrote (from CMake's install manifest) and nothing else.
- **Arch package**: `tools/package-arch.sh` (or
  `cmake --build build-rel --target package-arch`) builds
  `dist/geth-1.0.1-1-x86_64.pkg.tar.zst` + `dist/SHA256SUMS` from a
  `git archive` of a clean HEAD, with pinned, checksummed dependencies
  and **no network in `build()`**. `geth.install` prints a short RAPL
  note after install/upgrade and does nothing else.
- `namcap` clean on the PKGBUILD and the built package: no errors; the
  only warnings are the standard dependency heuristics about `gcc-libs`,
  which is the correct provider of `libstdc++.so.6` and
  `libgcc_s.so.1`.
- New man page: `docs/geth.1` (roff/mdoc) — synopsis, all flags, key
  bindings, files, environment, examples.

### Documentation

- The long reference content moved to `docs/CONFIG.md`,
  `docs/THEMES.md`, `docs/HARDWARE.md`, and `docs/DEVELOPMENT.md`; the
  README is now the front door (pitch, features, screenshots, hardware,
  install, usage, FAQ).
- Nine screenshots in `docs/screenshots/` captured live from a Konsole
  window running `geth --demo` (Spectacle, 1920×1080 screen);
  `tools/frame2png.py` and `--render-frame` remain for CI-style static
  frames.

### Testing

- **`GETH_TEST=1` save guard**: when set, `Config::save()` refuses to
  write anywhere under the real `~/.config` (unit-tested: guard on,
  guard on + temp `XDG_CONFIG_HOME`, guard off).
- 30-minute Release soak with a key-cycling workload: RSS flat at
  1/10/20/30 minutes.

## 1.0.0 — Phase 5: Themes, robustness, packaging (2026-10-16)

First stable release. Adds theming, hardens the runtime, and ships
packaging.

### Themes

- **Four built-in 24-bit themes**: `cyberpunk-blue` (default),
  `matrix-green`, `crimson-red`, `amber`.
- **Custom themes** from `$XDG_CONFIG_HOME/geth/themes/<name>.toml` with
  `base = "…"` inheritance (built-in or custom — layering allowed),
  `fill_mix`, and all nine color fields plus six series colors. Invalid
  hex / unknown fields / a missing or broken base produce a warning (and a
  `geth: theme: …` stderr line) and fall back — never a crash.
- **Selection**: `[general] theme`, `--theme NAME`, `--list-themes` (marks
  the active one), and `T` in the UI to cycle (each press saves).
- **Background mode**: `[general] background = "transparent"` (default —
  GETH never paints a background or inverts text; a test asserts zero
  background SGR codes for every theme) or `"solid"` (paints the theme's
  `bg` across the screen — the only sanctioned background path).
- **Color fallback**: 256/16 mapping for every theme; the six series map to
  distinct 16-color slots when distinct slots exist. A test renders every
  theme in all three color modes.
- `packaging/themes/example.toml` documents every field.

### Carry-over fixes

- **GPU renames are global**: `r` on any GPU panel (or `[gpus."…"].name`)
  applies to both GPU panels, the temps/power legends, the compact titles,
  and the header for that PCI bus. A per-panel `title` still overrides for
  that one panel.
- **Header title**: `[general] title` (non-empty) replaces the auto
  "<CPU> & <GPU>" title.
- **Footer**: `q quit · space pause · t window · e panels · ? help`, plus
  `↑↓ scroll · ▲ N more ▼` while the page overflows; truncated from the
  right.
- **Integrated-GPU memory panel**: GTT is the primary filled series, the
  BIOS VRAM carve-out is a line, scaled to the `{1, 1.5}×2^k` ceiling of
  `max(vram_total, gtt_total)`. Discrete GPUs unchanged.

### Robustness

- **Suspend / clock jumps**: a gap between samples greater than 5× the
  interval records NaN for all rate-based series (CPU usage, counter power,
  disk, network) for that tick, instead of a bogus spike.
- **Signals**: SIGINT, SIGTERM, SIGHUP, SIGSEGV all restore the terminal
  (SIGHUP via a custom handler; the rest via FTXUI's traps).
- **Soak**: 30-minute Debug (ASan+UBSan) run in a pty cycling
  theme/window/columns/manager — sanitizer output empty, RSS flat.
- **Startup**: process start → first full draw measured and well under the
  150 ms target.

### Packaging and docs

- `cmake --install` installs the binary, `share/geth/themes/example.toml`,
  and the udev rule under `share/geth/` (not `/etc`).
- `GETH_BINARY_NAME` CMake option (default `geth`) to avoid the Go Ethereum
  name clash (build as e.g. `gethmon`).
- `packaging/arch/PKGBUILD` (from a local git checkout).
- README rewritten: features, hardware matrix, keys, config reference,
  themes + custom guide, RAPL permissions (RAPL exists on AMD Zen; it is
  unreadable (EACCES), not absent), hybrid-laptop dGPU sleep,
  troubleshooting, and a development guide.
- `tools/collect-report.sh` writes a support report (`uname -a`,
  `--version`, `--probe`, 5× `--dump`, `--layout-report`, a full
  `--render-frame`) — no sudo, changes nothing.

## 0.4.0 — Phase 4: panel management + config file (2026-10-16)

- **Config file**: toml++-based `$XDG_CONFIG_HOME/geth/config.toml` with
  `[general]`, `[[panels]]`, `[gpus."…"]`; merge rules (absent hardware
  kept in file, new hardware inserted visible, file order honored); atomic
  save (tmp + rename); a broken file is never overwritten; `--config` /
  `--no-save`.
- **Panel manager** (`e`): show/hide, `K`/`J` reorder, `r` rename, series
  submenu, `d` reset. Focus via `Tab`/`Shift-Tab`, `x`, and mouse click.
  Setting cycles (`+`/`-` interval, `t` window, `c` columns) each save with
  a "saved" toast. The `?` help overlay. `--render-frame --script` event
  replay.
- **Rendering carry-overs**: distinct same-device series colors (lighter
  variant), filled areas, steep-line Bresenham, watts axis (nice quarter,
  20 W floor), byte-rate axis, and the idle-redraw loop (1 fps, ~0.5 % CPU
  at 250×70).

## 0.3.0 — Phase 3: responsive layout, compact panels, scrolling, tools (2026-10-15)

- **Responsive layout engine** (`ui/layout.{hpp,cpp}`): a pure
  `compute_layout(term_w, term_h, panels, prefs)` returning a rect and
  render mode per panel — no FTXUI, no I/O, re-run on resize. Panel modes
  Full and Compact; selection prefers all-Full, then maximizes the smallest
  graph height, then total graph area, then fewer columns; else scroll at
  the minimum Compact height. `--columns N` pins the count.
- **LayoutCache**: recomputed only on resize; zero heap allocations when the
  size is unchanged.
- **Scrolling** (only when overflowing): line/page/Home/End + mouse wheel,
  row-boundary snapping, footer `↑↓ scroll` and `N more` indicators. `t`
  cycles the graph window 30s → 1m → 2m → 5m → 10m.
- **Tools**: `--layout-report COLSxROWS`, `--perf`, `--columns N`.
- **Carry-over fixes**: APU package power survives the probe → hardware →
  default-page chain; the series palette; absolute memory/VRAM scale
  (`{1, 1.5}×2^k` ceiling); swap drawn as a line.

## 0.2.1 — Phase 2.1: fix the rendering bugs (2026-10-15)

- **Frame geometry self-check**: `--render-frame` verifies its output is
  exactly ROWS lines × COLS display columns (wcwidth) before printing;
  nonzero exit + offending line numbers on failure.
- **Braille graphs**: correct 2×4 dot table; each cell's color is the
  topmost dot (line beats fill); graphs plot history over a virtual clock
  (`--demo --warmup N`) so the 60 s window fills; a NaN gap renders empty,
  never 0.
- **Axes and labels**: consistent data-area column; compact labels without
  trailing zeros; byte-rate auto-scale as a power of two × 1024ⁿ (1 MiB/s
  floor).
- **Legends and titles**: two-column legends with separated, right-aligned
  values; no space after the color bar; VRAM panels titled "<GPU> ·
  Memory".

## 0.2.0 — Phase 2: FTXUI dashboard + CPU power sources (2026-10-15)

- **Interactive dashboard** (default mode): FTXUI v6.1.9 via FetchContent
  (pinned by tag + SHA256). Data-driven panel model; KDE-style panels with
  rounded borders, braille 2×4 graphs, fill+line, legends, and info rows.
  The default page is CPU, per-GPU usage, Memory, per-GPU VRAM,
  Temperature, Power, Network, Disk.
- **Transparency**: no background color is ever emitted (`Color::Default`
  only); a test walks the SGR stream and asserts it.
- The timer thread only posts `Event::Custom`; all sampling/rendering run
  on the UI thread. Quit restores the terminal, including after the crash
  signals FTXUI traps. `space` pauses the display while sampling continues.
- `--render-frame COLSxROWS [--demo] [--warmup N] [--ansi]`, `--stats`,
  `--color truecolor|256|16`. Never crashes at any size; below 40×12 a
  centered notice.
- **CPU package power**: RAPL `energy_uj` (multi-socket), zenpower
  (instantaneous uW), amd_energy (cumulative uJ); the probe reports EACCES
  with `CapEff` and the `setcap` / `install-udev` fix targets; without RAPL
  the iGPU PPT is lifted as "APU Package (PPT)".
- Zero heap allocations per tick after warm-up (counting-operator-new
  test); `--demo --dump --count 4000` keeps `Private_Dirty` flat. GTT and
  `pci_bus_id` in `--dump`/`--json`; NVML enumeration only under `--probe`.

## 0.1.0 — Phase 1: hardware tracking engine (2026-10-15)

- **Collectors**: CPU usage/frequency, CPU temperature (table-driven
  k10temp/zenpower/coretemp/x86_pkg_temp/acpitz), CPU package power
  (RAPL → amd_energy → zenpower → APU PPT lift), memory, per-device disk
  rates, per-interface net rates, GPUs via a registry of backends.
- **GPU architecture**: PCI discovery (`/sys/bus/pci/devices`, class 0x03)
  as the single source of truth; a `GpuBackend` interface; the registry as
  the only backend list with unclaimed-device reporting; strict
  NVML↔sysfs matching by normalized PCI bus id (no positional fallback); a
  lazy NVML session with 10-suspended-tick shutdown; AMD `gpu_metrics` APU
  detection; the integrated-GPU PPT split into CPU package power; a
  streaming `pci.ids` subset loader.
- 49 unit tests (doctest); ASan+UBSan-clean Debug and clean Release builds.
