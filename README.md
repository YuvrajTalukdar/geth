# GETH — Geth Engine for Tracking Hardware

A fast, lightweight terminal system monitor for Linux, written in C++20.
Phase 1 (this tree) is the **data collection layer** with a text and a JSON
output mode; the FTXUI dashboard comes in a later phase and reads the same
`Collector`/`History` API.

No external runtime dependencies. No subprocesses: everything is read from
`/proc`, `/sys`, or NVML (loaded with `dlopen`, never linked).

## Build

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Debug        # ASan+UBSan, -O1 (default)
# or
cmake -B build -DCMAKE_BUILD_TYPE=Release      # -O2
cmake --build build -j
ctest --test-dir build                         # unit tests
```

## Run

```sh
./build/geth --version
./build/geth --probe          # which sources work on this machine, and why
./build/geth --interval 1     # human-readable samples (default mode)
./build/geth --json --count 5 # JSON, one object per sample
./build/geth --demo           # synthetic data, no hardware touched
```

## CPU power permissions

On kernel ≥ 5.10 `energy_uj` is root-readable. `--probe` reports this with a
fix hint; the permanent fix is the udev rule:

```sh
sudo cp packaging/99-geth-rapl.rules /etc/udev/rules.d/
sudo udevadm trigger --subsystem-match=powercap
```

## Layout

```
src/core/       snapshot model, source interface, ring-buffer history, collector
src/sources/    one file per metric family (cpu, memory, power, temps, gpus, disk, net, demo)
src/util/       sysfs fd wrapper, pci.ids lookup, text parsing, formatting
tests/          doctest unit tests + fixture files
packaging/      udev rule for RAPL access
```
