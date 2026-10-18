# GETH — Development

## Dependencies

CMake ≥ 3.20 and a C++20 compiler (GCC ≥ 11 or Clang ≥ 14 tested). The only
build-time dependencies, fetched by CMake via FetchContent and pinned to exact
versions with SHA256 checksums, are:

- [FTXUI v6.1.9](https://github.com/ArthurSonzogni/FTXUI/releases/tag/v6.1.9) — terminal UI (screen + component)
- [toml++ v3.4.0](https://github.com/marzer/tomlplusplus/releases/tag/v3.4.0) — config file and custom themes (header-only)
- [doctest v2.4.11](https://github.com/doctest/doctest/releases/tag/v2.4.11) — unit tests (header-only)

One system library (found by CMake, not fetched): **zlib** — the pixel
renderer's image payloads (Kitty `o=z`) and the `--render-png` output
(IDAT). Per distro: Arch `zlib`, Debian/Ubuntu `zlib1g-dev`, Fedora
`zlib-devel`.

Nothing is installed system-wide; everything lands in the build directory.
Per-distro build prerequisites:

| Distro | Packages |
|---|---|
| Arch | `base-devel cmake git` |
| Debian/Ubuntu | `build-essential cmake git` |
| Fedora | `gcc-c++ cmake git` |

## Builds

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug        # ASan+UBSan, -O1 (default)
# or
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release  # -O2
cmake --build build -j
ctest --test-dir build --output-on-failure     # unit tests
```

The Debug build is sanitized by default so memory and UB bugs surface while
developing. The project version is defined **once** in `CMakeLists.txt`
(`project(geth VERSION …)`); the packaging scripts check the Arch PKGBUILD
against it and fail on a mismatch. The license is the single CMake variable
`GETH_LICENSE` (SPDX identifier), consumed by the install rule and the
packaging checks.

### Binary name (Go Ethereum clash)

On a system that also runs Go Ethereum's `geth` client, the two binaries
clash on the name. Build and install GETH under a different name with the
`GETH_BINARY_NAME` CMake option (default `geth`):

```sh
cmake -B build-rel -DCMAKE_BUILD_TYPE=Release -DGETH_BINARY_NAME=geth-engine
```

The option changes the output/installed name (and the man page name); the
CMake target stays `geth` (the `setcap`/`install-udev` targets reference it).

### Install / uninstall (source installs)

```sh
cmake --install build-rel --prefix /usr/local        # or ~/.local
cmake --build build --target uninstall               # removes exactly the manifest files
```

The install places: `bin/<name>`, `share/geth/themes/example.toml`,
`share/geth/99-geth-rapl.rules` (shipped, **not** activated),
`share/man/man1/<name>.1.gz`, `share/doc/geth/{README,CHANGELOG}.md`, and
`share/licenses/<name>/LICENSE`. `uninstall` removes every file this install
wrote (from CMake's install manifest) and nothing else — in particular never
the user config in `~/.config/geth`.

## Test-safety guard

Every test, soak, or frame render must use a temporary `XDG_CONFIG_HOME`
(e.g. `mktemp -d`). Enforced in the binary: when `GETH_TEST=1` is set,
`Config::save()` refuses to write anywhere under the real `~/.config`.

## Layout

```
src/core/       snapshot model, source interface, ring-buffer history, collector
src/sources/    one file per metric family (cpu, memory, cpu_power, temps,
                gpu_backend + gpu_amd/gpu_nvidia, gpu_registry, disk, net, demo)
src/ui/         dashboard: frame renderer, layout, data-driven panel specs,
                theme, config file (toml++), panel manager + app loop,
                chart rasters + image backends (kitty/sixel), terminal
                probe, PNG composer + embedded glyphs
src/util/       sysfs fd wrapper, pci.ids lookup, text parsing, formatting,
                short names, stop/signal handling, terminal (SIGHUP restore)
tests/          doctest unit tests + fixture files
packaging/      udev rule for RAPL access, example custom theme, Arch PKGBUILD
tools/          collect-report.sh (support report), package-arch.sh (Arch package),
                frame2png.py (screenshots), gen_glyphs.py (embedded glyph set),
                measure_pixel_perf.py (render-cost measurement)
docs/           CONFIG.md, THEMES.md, HARDWARE.md, DEVELOPMENT.md, geth.1 (man page)
```

## Adding a GPU backend

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

## Tests

```sh
cmake --build build -j
ctest --test-dir build --output-on-failure
# or run the binary directly (raw doctest summary):
./build/geth_tests
./build/geth_tests --test-case="*"   # filter with doctest flags
```

The Debug build is ASan+UBSan, so the suite catches memory and UB issues.
Key invariants under test: the "no background" rule (zero background SGR
codes in transparent mode for every theme), the 16-color series distinctness,
every built-in theme in all three color modes, theme
parsing/inheritance/fallback, the suspend-gap NaN blanking, config
round-trip, the `GETH_TEST=1` save guard, and the UI's rename/title/footer
behavior.

## Rendering a frame without a terminal

`--render-frame COLSxROWS` renders a single frame and exits — ideal for CI,
screenshots, and layout debugging:

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

`--render-png COLSxROWS` goes one step further: it composites the frame —
the generated background, the text glyphs (an embedded DejaVu Sans Mono
coverage set, `src/ui/glyphs.{hpp,cpp}`, regenerated by
`tools/gen_glyphs.py`) and, in pixels mode, each graph's raster — into an
8-bit RGB PNG on stdout, using the C++ composer in `src/ui/png.cpp`
(IHDR / IDAT via zlib / IEND, no external PNG dependency):

```sh
./build/geth --render-png 200x56 --demo --renderer pixels > frame.png
./build/geth --render-png 200x56 --demo --renderer text --theme matrix-green
```

`--probe-terminal` reports what the terminal speaks (Kitty graphics
protocol, Sixel, the cell pixel size) and the renderer the startup rules
would choose — the same probe the dashboard runs on launch.

Performance: `tools/measure_pixel_perf.py` runs the dashboard in a pty
against a fake terminal at a fixed size and 1 Hz and reports wall time,
CPU % (rusage of the geth process) and bytes/s, for the pixel and the
text renderer:

```sh
python3 tools/measure_pixel_perf.py build-rel/geth 70x250 30
```

Screenshots: `tools/frame2png.py` renders a `--render-frame --ansi` capture
to a PNG on an exact cell grid (DejaVu Sans Mono, truecolor, procedural dark
gradient background) — useful for CI and layout debugging. The README
gallery is live captures instead: `geth --demo` running in a Konsole window,
shot with Spectacle on a 1920×1080 screen (205×58 for the theme/manager/
help shots, 236×71 wide, 111×31 narrow, 85×25 compact).

## Collecting a support report

`tools/collect-report.sh` builds the Release binary if needed and writes
`geth-report-<hostname>.txt` containing `uname -a`, `geth --version`,
`geth --probe`, five `--dump` samples, the `--layout-report 160x48`
decision, and a full `--render-frame 160x48` frame. It needs no root and
changes nothing:

```sh
tools/collect-report.sh                 # -> geth-report-<host>.txt
tools/collect-report.sh /tmp/report.txt # -> /tmp/report.txt
```

Share the resulting file when reporting a problem.
