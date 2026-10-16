# GETH — Geth Engine for Tracking Hardware

A fast, lightweight terminal system monitor for Linux, written in C++20.
The default mode is an interactive, multi-panel dashboard; `--dump` and
`--json` print the same data as human-readable or JSON samples for
scripting, and `--render-frame` emits a single frame for screenshots and
tests.

No subprocesses: everything is read from `/proc`, `/sys`, or NVML (loaded
with `dlopen`, never linked). The only build-time dependencies, fetched by
CMake via FetchContent and pinned to exact versions, are
[FTXUI v6.1.9](https://github.com/ArthurSonzogni/FTXUI/releases/tag/v6.1.9)
(SHA256 `45819c1e54914783d4a1ca5633885035d74146778a1f74e1213cdb7b76340e71`),
[toml++ v3.4.0](https://github.com/marzer/tomlplusplus/releases/tag/v3.4.0)
(SHA256 `8517f65938a4faae9ccf8ebb36631a38c1cadfb5efa85d9a72e15b9e97d25155`,
header-only, for the config file and custom themes), and doctest 2.4.11 for
the unit tests.

## Contents

- [Features](#features)
- [Screenshots](#screenshots)
- [Hardware support](#hardware-support)
- [Build](#build)
- [Install](#install)
- [Run](#run)
- [Keys](#keys)
- [Configuration](#configuration)
- [Themes](#themes)
  - [Built-in themes](#built-in-themes)
  - [Custom themes](#custom-themes)
  - [Background mode](#background-mode)
  - [Color fallback](#color-fallback)
- [CPU power permissions](#cpu-power-permissions)
- [Hybrid laptops: discrete-GPU sleep](#hybrid-laptops-discrete-gpu-sleep)
- [Troubleshooting](#troubleshooting)
- [Collecting a support report](#collecting-a-support-report)
- [Development](#development)

## Features

- **Multi-panel dashboard** — CPU, GPU (usage + memory), RAM/swap,
  temperatures, CPU package power, network, and disk, each in its own
  bordered panel with a live graph. Panels are data-driven: the layout
  reflows to the terminal size and column count, and each panel degrades
  gracefully (full graph → compact → hidden) as space shrinks.
- **Per-GPU panels** — one usage panel and one memory panel per GPU, for
  every detected card (AMD and NVIDIA, integrated and discrete). On an
  integrated AMD GPU the memory panel plots **GTT** (the aperture the OS
  actually uses) as the filled series and the fixed BIOS **VRAM** carve-out
  as a line, scaled to `max(vram_total, gtt_total)`.
- **GPU renames are global** — renaming a GPU in the panel manager (`r`)
  applies the name to every panel, legend, and the header for that PCI bus.
- **Themes** — four built-in 24-bit themes, custom themes from
  `$XDG_CONFIG_HOME/geth/themes/*.toml` (with inheritance and `fill_mix`),
  live theme cycling (`T`), and automatic 256/16-color fallback.
- **Transparent by default** — GETH never paints a background color or
  inverts text, so it sits cleanly over any terminal theme. An optional
  `background = "solid"` mode paints the theme's `bg` color across the
  screen (the single sanctioned background path).
- **No subprocesses, low overhead** — everything is read in-process from
  `/proc`, `/sys`, or NVML. Render time is measured and reported with
  `--stats`.
- **Robust** — the terminal is restored on quit *and* on crash signals
  (SIGINT/SIGTERM/SIGHUP/SIGSEGV); a gap between samples (suspend/clock
  jump) blanks rate-based series to NaN for that tick rather than showing a
  bogus spike.
- **Scriptable** — `--dump`, `--json`, `--probe`, `--render-frame`,
  `--layout-report`, `--perf`, and `--script` for automation and testing.

## Screenshots

Render any frame with `--render-frame` (use `--demo` for synthetic data, or
omit it to capture this machine):

```sh
./build-rel/geth --render-frame 160x48 --ansi > frame.txt
```

<!-- Screenshot placeholders — replace with rendered images: -->

| Cyberpunk Blue (default) | Matrix Green |
|---|---|
| ![cyberpunk-blue](review/screenshots/cyberpunk-blue.png) | ![matrix-green](review/screenshots/matrix-green.png) |

| Crimson Red | Amber |
|---|---|
| ![crimson-red](review/screenshots/crimson-red.png) | ![amber](review/screenshots/amber.png) |

> **Note:** the screenshot images are placeholders. Render them with
> `--render-frame 160x48 --ansi --theme <name>` and convert to PNG, or drop
> terminal captures into `review/screenshots/`.

## Hardware support

All metrics are read in-process (no `lshw`, `sensors`, `nvml` CLI, etc.).
`--probe` lists every source on the current machine, whether it is
available, and why not — with fix hints.

| Component | Source | Notes |
|---|---|---|
| CPU usage + frequency | `/proc/stat`, cpufreq `scaling_cur_freq` | AMD Zen (tested: Ryzen 5 5600H). Logical-CPU count from `/proc/stat`. |
| CPU temperature | `hwmon` (`k10temp` on AMD) | First usable `temp*_input`. |
| CPU package power | RAPL → `amd_energy` → `zenpower` → APU PPT fallback | See [CPU power permissions](#cpu-power-permissions). On an AMD APU with RAPL unreadable, the integrated GPU's **PPT** supplies package power (it already includes the iGPU). |
| RAM / swap | `/proc/meminfo` | RAM used = `MemTotal - MemAvailable` (KDE's definition). |
| Integrated GPU (AMD) | `amdgpu` sysfs (`gpu_busy_percent`, `mem_info_vram_used`, GTT, clocks, PPT) | Detected by PCI id; APU format via `gpu_metrics`. |
| Discrete GPU (NVIDIA) | NVML (loaded via `dlopen`, never linked) | Usage, memory, power, clocks, temperature, fan. Session opens lazily and drops after 10 suspended ticks (see [hybrid laptops](#hybrid-laptops-discrete-gpu-sleep)). |
| Disk | `/proc/diskstats` | Per-device read/write byte rates (sector counters × 512). |
| Network | `/proc/net/dev` | Per-interface rx/tx byte rates; `lo` and virtual interfaces excluded. |

### Tested / target machines

| Machine | CPU | iGPU | dGPU | Status |
|---|---|---|---|---|
| Dev machine | AMD Ryzen 5 5600H (APU) | Cezanne (Radeon Vega Mobile) | NVIDIA GeForce RTX 3050 Laptop | **Tested** (this tree is developed and tested here; RAPL is unreadable → APU PPT supplies CPU power) |
| Hybrid laptop | AMD Ryzen 7 7840HS | Radeon 780M | NVIDIA GeForce RTX 5060 Laptop | Target |
| Desktop | AMD Ryzen 7 5800X (no iGPU) | — | NVIDIA GeForce RTX 3070 | Target |

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug        # ASan+UBSan, -O1 (default)
# or
cmake -B build -DCMAKE_BUILD_TYPE=Release      # -O2
cmake --build build -j
ctest --test-dir build                         # unit tests
```

FTXUI, toml++, and doctest are fetched into the build directory; nothing is
installed system-wide. The Debug build is sanitized by default so memory and
UB bugs surface while developing.

### Binary name (Go Ethereum clash)

On a system that also runs Go Ethereum's `geth` client, the two binaries
clash on the name. Build and install GETH under a different name with the
`GETH_BINARY_NAME` CMake option (default `geth`):

```sh
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DGETH_BINARY_NAME=gethmon
cmake --build build-rel -j
# ./build-rel/gethmon ...
```

The option only changes the installed binary's `OUTPUT_NAME`; the CMake
target stays `geth` (the `setcap`/`install-udev` targets reference it).

## Install

```sh
cmake --install build-rel --prefix /usr/local
```

Installs:

- `bin/geth` (or the name set by `GETH_BINARY_NAME`)
- `share/geth/themes/example.toml` (a fully documented custom-theme example)
- `share/geth/99-geth-rapl.rules` (the udev rule, for
  `--target install-udev` — **not** installed into `/etc` by the generic
  install; the udev target copies it into the udev rules directory)

The udev rule and `setcap` are privilege-escalation steps and are run by
you, explicitly (see [CPU power permissions](#cpu-power-permissions)).

## Run

```sh
./build/geth                        # interactive dashboard UI (default)
./build/geth --probe                # which sources work on this machine, and why
./build/geth --dump --interval 1000 # human-readable samples
./build/geth --json --count 5       # JSON, one object per sample
./build/geth --demo                 # synthetic data, no hardware touched
./build/geth --render-frame 160x48  # render one frame at that size and exit
./build/geth --render-frame 120x40 --demo --warmup 60
./build/geth --render-frame 160x48 --ansi   # full SGR color codes
./build/geth --stats                # print render-time + alloc stats on exit
./build/geth --color 256            # force a color mode: truecolor | 256 | 16
./build/geth --theme matrix-green   # select a theme (see --list-themes)
./build/geth --list-themes          # list themes, mark the active one
./build/geth --config path.toml     # use an alternate config file
./build/geth --no-save              # treat the config file as read-only
./build/geth --render-frame 160x48 --demo --script "e,down,space"
                                    # render one frame after replaying those keys
```

Terminals below 40×12 get a centered "terminal too small" notice instead of a
broken frame.

## Keys

Press `?` in the UI for the full list.

| Key | Action |
|---|---|
| `q` / `Esc` / `Ctrl-C` | Quit (the terminal is restored, even after a crash signal) |
| `space` | Pause / resume the display (sampling continues) |
| `Tab` / `Shift-Tab` | Move focus between visible panels (accent border) |
| `x` | Hide the focused panel |
| mouse click | Focus the panel under the cursor |
| mouse wheel / `↑` `↓` / `j` `k` | Scroll (when the page overflows) |
| `e` | Open the **panel manager** |
| `?` | Help overlay |
| `+` `=` / `-` `_` | Change the sampling interval |
| `t` | Cycle the graph window (30s → 1m → 2m → 5m → 10m) |
| `c` | Cycle the column count |
| `T` | Cycle the **theme** (saved to the config) |

In the **panel manager** (`e`): `↑` `↓` / `j` `k` select, `space` show/hide a
panel, `K` `J` (or the mouse) reorder, `r` rename a GPU (applied globally to
that bus — see [Configuration](#configuration)), `enter` then `space` hide a
series, and `d` reset everything to the defaults. Every manager and setting
change saves the config file with a short "saved" toast.

## Configuration

The dashboard reads `$XDG_CONFIG_HOME/geth/config.toml` (or
`~/.config/geth/config.toml` when `XDG_CONFIG_HOME` is unset). On the first
run there is no file: the defaults are used, and the file is written only
the first time a setting actually changes (never on a bare launch).
`--config PATH` points at a different file, and `--no-save` makes it
read-only (changes apply for the session only, and no "saved" toast appears).
A config the app wrote is a good starting point to edit by hand.

Schema — unknown keys (and malformed values) are warned about on `stderr`
and ignored, never an error:

```toml
[general]
interval_ms = 1000      # 250 | 500 | 1000 | 2000 | 5000
window = "1m"           # "30s" | "1m" | "2m" | "5m" | "10m"
columns = "auto"        # "auto" | 1..8
title = ""              # header title override ("" = auto: CPU & GPU names)
theme = "cyberpunk-blue"# built-in or custom theme name (see --list-themes)
background = "transparent"  # "transparent" | "solid" (paints the theme bg)

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
```

Panel ids: `cpu`, `mem`, `temps`, `power`, `net`, `disk`, and
`gpu.<bus>.usage` / `gpu.<bus>.vram` (one usage + one VRAM panel per GPU).
Series ids look like `cpu.usage`, `mem.ram_used`, `gpu.<bus>.vram`.

A per-panel `title` overrides the default for that one panel only; the
`[gpus."…"].name` rename is what changes *every* occurrence of the GPU's
name (both its panels, the temps/power legends, and the header).

Merge rules on load: a panel whose hardware is absent stays in the file but
is not shown (it appears as "(not present)" in the panel manager, so its
settings are kept for the day the hardware returns); a panel whose hardware
is new is inserted **visible** at its default position; a panel present in
both keeps the file's order and settings. Saves are atomic (a temp file in
the same directory, then `rename`), so a crash never leaves a half-written
config, and a stale `.tmp` from a crashed save is dropped on the next load.
A file that does not parse is never overwritten: the defaults are used, the
footer shows the parse error, and `save()` refuses.

## Themes

Themes are 24-bit palettes. GETH auto-detects the terminal's color depth
(`truecolor` / `256` / `16`) and falls back gracefully — see
[Color fallback](#color-fallback).

### Built-in themes

| Name | Accent | Character |
|---|---|---|
| `cyberpunk-blue` (default) | `#00E5FF` | Cyan/blue on dark |
| `matrix-green` | `#39FF14` | Terminal green |
| `crimson-red` | `#FF2E4D` | Warm red |
| `amber` | `#FFB000` | Monochrome amber |

Select one with `[general] theme = "…"`, `--theme NAME`, or press `T` in the
UI to cycle (each press saves). `--list-themes` lists every selectable theme
(built-in + custom) and marks the active one.

### Custom themes

Drop a `.toml` file in `$XDG_CONFIG_HOME/geth/themes/` (the filename, without
`.toml`, is the theme name). Start from the shipped example, which documents
every field:

```sh
cp packaging/themes/example.toml ~/.config/geth/themes/solar.toml
```

A theme may set any of these (all colors are `#RRGGBB`):

| Field | Meaning |
|---|---|
| `base` | Inherit from another theme (built-in or custom — layering is allowed). Omit for a fully explicit theme. |
| `accent` | The "live" foreground: current values, selected rows, the title |
| `title` | Panel titles (defaults to `accent` when omitted) |
| `text` | Body text: labels, numbers, graph legends |
| `label_dim` | Secondary/dim text: units, hidden markers, the footer |
| `border` | Panel borders |
| `grid` | Graph grid lines |
| `axis_text` | Graph axis labels |
| `warn` | Warning toast / config-error foreground |
| `bg` | Solid-mode screen background (only used when `background = "solid"`) |
| `fill_mix` | `[0, 1]` — how much of the series color the graph "fill" keeps (default `0.45`) |
| `series_1` … `series_6` | The six graph series colors, in order (panels wrap through them) |

A field left out (or set to a bad value) falls back to the base theme's
value, so a theme can override just a few. **Invalid hex values and unknown
keys never crash the app**: each produces a warning toast (and a
`geth: theme: …` line on `stderr`) and that field keeps its inherited value.
An unknown `base`, a missing file, or an unparseable file means the theme
resolves to nothing and is skipped (with a warning), never a crash.

### Background mode

`[general] background` is `"transparent"` (default) or `"solid"`.

- **transparent** — GETH never paints a background color or inverts text; it
  is transparent over any terminal theme. A unit test asserts the rendered
  output contains zero background SGR codes for every theme.
- **solid** — the theme's `bg` color is painted across the whole screen. This
  is the *only* place GETH may paint a cell background.

### Color fallback

For terminals without 24-bit color, each 24-bit palette color is mapped to
the nearest 256-color (or 16-color) entry. The six series colors are mapped
so that **no two distinct series share the same 16-color slot** when distinct
slots exist, keeping adjacent panels distinguishable. A test renders every
theme in all three color modes and asserts the series stay distinct.

## CPU power permissions

CPU package power comes from a chain of sources, in order: **RAPL** (the
`powercap` `energy_uj` counters), then `amd_energy`, then `zenpower`, and
finally — on an AMD APU — the **integrated GPU's PPT**, whose APU package
power already includes the iGPU, so the "APU Package (PPT)" line stays
correct.

RAPL **exists on AMD Zen** too — it is the same `powercap`
`intel_rapl_common` interface. On kernel ≥ 5.10 the `energy_uj` files are
`0400 root:root`, so without extra permissions GETH reads **EACCES**
(unreadable) — *not* "absent". `--probe` reports EACCES, the binary's
effective capabilities (`CapEff`; `cap_dac_read_search` is bit 2, so a failed
or stale `setcap` is visible), and which source is supplying CPU package
power in the meantime.

Two permanent fixes, one command each (run from the build directory; each
needs `sudo` and is run by you, not by GETH):

```sh
sudo cmake --build build --target install-udev  # udev rule for energy_uj
sudo cmake --build build --target setcap        # cap_dac_read_search on the binary
```

`install-udev` is the more surgical option: it installs
`packaging/99-geth-rapl.rules`, which makes only the `package-*` powercap
domains world-readable. `setcap` grants `cap_dac_read_search` on the binary,
which bypasses file permission checks more broadly — prefer the udev rule on
shared machines. As a last resort, run `geth` as root.

## Hybrid laptops: discrete-GPU sleep

On Optimus/hybrid laptops the discrete NVIDIA GPU is powered down when idle.
GETH handles this without any user action:

- NVML is **never linked**; it is `dlopen`ed on first use, so a machine with
  no NVIDIA driver (or no NVML) still runs.
- The NVML session **opens lazily** on the first tick where a card is not
  suspended, and is **dropped after 10 consecutive suspended ticks** (the
  card is back to sleep). Waking the card is what costs power, so GETH does
  not poll a sleeping card.
- A suspended card shows its last known values (dimmed) rather than zeroes,
  so the panel does not flicker to 0% while the dGPU is asleep.

## Troubleshooting

- **`--probe` says a source is unavailable.** Read the `why` and `fix` lines —
  they name the exact path, the error, and the remedy. Common cases:
  - *CPU power: EACCES on `energy_uj`* → install the udev rule or `setcap`
    (see [CPU power permissions](#cpu-power-permissions)); until then the APU
    PPT supplies package power.
  - *NVIDIA: no NVML* → the `nvidia` driver's `libnvidia-ml.so` is not
    present (driver not installed, or a headless/datacenter driver without
    it). Install the full driver, or the NVIDIA panels stay absent.
  - *AMD GPU: `nouveau` in use* → GETH reads the `amdgpu` sysfs interface; if
    the GPU is driven by `nouveau`, the AMD GPU panels have no data. Blacklist
    `nouveau` and use `amdgpu`.
  - *Temperature: no `hwmon`* → the `k10temp` (AMD) or equivalent sensor
    module is not loaded; the temps panel shows what is available.
- **The frame looks cut off or "terminal too small".** The UI needs at least
  40×12; below that it shows a centered notice. Resize the terminal, or use
  `--render-frame COLSxROWS` to capture a specific size.
- **A panel shows "—" or is missing.** The underlying hardware/source is
  absent on this machine (e.g. no network interface, a dGPU that is asleep).
  `--probe` confirms what is present.
- **Colors look wrong / no color.** Force a mode with `--color truecolor|256|16`
  and ensure `TERM` advertises the right capabilities.

## Collecting a support report

`tools/collect-report.sh` builds the Release binary if needed and writes
`geth-report-<hostname>.txt` containing `uname -a`, `geth --version`,
`geth --probe`, five `--dump` samples, the `--layout-report 160x48` decision,
and a full `--render-frame 160x48` frame. It needs no root and changes
nothing:

```sh
tools/collect-report.sh                 # -> geth-report-<host>.txt
tools/collect-report.sh /tmp/report.txt # -> /tmp/report.txt
```

Share the resulting file when reporting a problem.

## Development

### Layout

```
src/core/       snapshot model, source interface, ring-buffer history, collector
src/sources/    one file per metric family (cpu, memory, cpu_power, temps,
                gpu_backend + gpu_amd/gpu_nvidia, gpu_registry, disk, net, demo)
src/ui/         dashboard: frame renderer, layout, data-driven panel specs,
                theme, config file (toml++), panel manager + app loop
src/util/       sysfs fd wrapper, pci.ids lookup, text parsing, formatting,
                short names, stop/signal handling, terminal (SIGHUP restore)
tests/          doctest unit tests + fixture files
packaging/      udev rule for RAPL access, example custom theme
tools/          collect-report.sh (support-report generator)
```

### Adding a GPU backend

GPU discovery is backend-agnostic: `gpu_registry.cpp` enumerates PCI GPUs and
`pci_discovery.cpp` classifies them; each vendor backend implements the
`GpuBackend` interface in `gpu_backend.hpp`:

1. **Claim** — `claim(GpuInfo)` returns true if this backend can drive the
   device (NVIDIA by NVML presence + PCI id; AMD by `amdgpu` sysfs).
2. **Sample** — `sample(GpuInfo, Snapshot&)` fills the GPU's usage, memory,
   power, clocks, and temperature. Keep all I/O in `sysfs_file.cpp` / NVML
   wrappers; never spawn a subprocess.
3. **Name** — provide a human name (NVIDIA from NVML; AMD from
   `/usr/share/hwdata/pci.ids`).

The backend is selected by `gpu_backend.cpp` at probe time. Add the new
`.cpp` to `GETH_SOURCES` in `CMakeLists.txt` and a unit test under `tests/`.

### Tests

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure
# or run the binary directly:
./build/geth_tests                 # all doctest cases
./build/geth_tests --test-case="*" # filter with doctest flags
```

The Debug build is ASan+UBSan, so the suite catches memory and UB issues.
Key invariants under test: the "no background" rule (zero background SGR
codes in transparent mode for every theme), the 16-color series
distinctness, theme parsing/inheritance/fallback, the suspend-gap NaN
blanking, config round-trip, and the UI's rename/title/footer behavior.

### Rendering a frame without a terminal

`--render-frame COLSxROWS` renders a single frame and exits — ideal for
CI, screenshots, and layout debugging:

```sh
./build/geth --render-frame 160x48 --demo --warmup 60        # plain text
./build/geth --render-frame 160x48 --ansi --theme amber      # full SGR
./build/geth --render-frame 120x40 --demo --script "e,down,space"
                                                              # after keys
```

`--script <events>` replays a comma-separated list of synthetic events
(`enter`, `space`, `tab`, `up`/`down`/`left`/`right`, `click:X:Y`, or a
single character) before the frame is rendered, so a specific UI state
(manager open, a panel toggled, a panel focused) can be captured
deterministically. `--layout-report COLSxROWS` prints the layout decision
(candidate column counts, per-panel rects and modes) instead of a frame.
