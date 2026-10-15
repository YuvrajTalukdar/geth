# GETH — Geth Engine for Tracking Hardware

A fast, lightweight terminal system monitor for Linux, written in C++20.
The default mode is an interactive dashboard; `--dump` and `--json` print
the same data as text or JSON samples for scripting.

No subprocesses: everything is read from `/proc`, `/sys`, or NVML (loaded
with `dlopen`, never linked). The only build-time dependencies, fetched by
CMake via FetchContent and pinned to exact versions, are
[FTXUI v6.1.9](https://github.com/ArthurSonzogni/FTXUI/releases/tag/v6.1.9)
(SHA256 `45819c1e54914783d4a1ca5633885035d74146778a1f74e1213cdb7b76340e71`)
and doctest 2.4.11 for the unit tests.

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug        # ASan+UBSan, -O1 (default)
# or
cmake -B build -DCMAKE_BUILD_TYPE=Release      # -O2
cmake --build build -j
ctest --test-dir build                         # unit tests
```

FTXUI and doctest are fetched into the build directory; nothing is
installed system-wide.

## Run

```sh
./build/geth                        # interactive dashboard UI (default)
./build/geth --probe                # which sources work on this machine, and why
./build/geth --dump --interval 1    # human-readable samples
./build/geth --json --count 5       # JSON, one object per sample
./build/geth --demo                 # synthetic data, no hardware touched
./build/geth --render-frame 160x48  # render one frame at that size and exit
./build/geth --render-frame 120x40 --demo --warmup 60
./build/geth --stats                # print render-time + alloc stats on exit
./build/geth --color 256            # force a color mode: truecolor | 256 | 16
```

UI keys: `q` / `Esc` / `Ctrl-C` quit (terminal is restored, even after a
crash signal), `space` pauses the display while sampling continues. The UI
never paints a background color — it is transparent over any terminal theme
(and a unit test asserts this). Terminals below 40x12 get a centered
"terminal too small" notice instead of a broken frame.

## CPU power permissions

On kernel ≥ 5.10 the RAPL `energy_uj` counters are `0400 root:root`. Without
access, `--probe` reports EACCES, the binary's effective capabilities
(`CapEff`, so a failed or stale `setcap` is visible — `cap_dac_read_search`
is bit 2), and which source supplies CPU package power in the meantime: on
AMD APUs that is the integrated GPU's PPT, whose APU package power already
includes the iGPU, so the UI's "APU Package (PPT)" line stays correct.

Two permanent fixes, one command each from the build directory:

```sh
sudo cmake --build build --target install-udev  # udev rule for energy_uj
sudo cmake --build build --target setcap        # cap_dac_read_search on the binary
```

`install-udev` is the more surgical option: it installs
`packaging/99-geth-rapl.rules`, which makes only the `package-*` powercap
domains world-readable. `setcap` grants DAC read/search on the binary, which
bypasses file permission checks more broadly — prefer the udev rule on
shared machines.

## Layout

```
src/core/       snapshot model, source interface, ring-buffer history, collector
src/sources/    one file per metric family (cpu, memory, power, temps, gpus, disk, net, demo)
src/ui/         dashboard: frame renderer, data-driven panel specs, theme, app loop
src/util/       sysfs fd wrapper, pci.ids lookup, text parsing, formatting, short names
tests/          doctest unit tests + fixture files (parsers, names, UI, alloc counting)
packaging/      udev rule for RAPL access
```
