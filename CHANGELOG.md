# Changelog

All notable changes to GETH. The format follows the five-phase build plan.

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
