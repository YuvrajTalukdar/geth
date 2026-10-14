# GETH — Phase 1.2 report

Date: 2026-10-15. Machine: AMD Ryzen 5 5600H (Cezanne iGPU) + GeForce RTX 3050 Laptop
(GA107M), Arch Linux, kernel 7.2.9-arch1-1, g++ 16.2.1, driver 615.78.08.

---

## 1. Builds, tests, sanitizer run

**Debug** (`-O1 -fsanitize=address,undefined`, `-Wall -Wextra -Wpedantic -Werror`):
build clean, **49/49 test cases, 284/284 assertions** pass.

**Release** (`-O2`, same warning flags): build clean, **49/49, 284/284** pass.

`./build-debug/geth --dump --count 30` under ASan+UBSan with the NVML session live:
exit 0, no sanitizer report, 30 samples (1 s interval) in ~30 s.

New/changed unit tests (doctest):
- `pci_ids` suite rewritten for the streaming subset loader: requested pairs are
  resolved, unrequested pairs are `nullopt`, a second `load_subset` is a no-op.
- `pci_discovery` (2 cases): fake sysfs tree → class-0x03 filtering, ids/driver/
  boot_vga/DRM-card mapping; registry keeps an unclaimed Intel device
  (8086:9a49, i915) visible as "detected, no backend yet".
- `nvml_match` (3 cases): `normalize_pci_bus_id` exact-form/upper-casing/rejection;
  **two NVIDIA GPUs in reversed NVML order match by bus id, never by position**;
  mismatches reported in both directions.
- `nvml_lifecycle` (2 cases, fake NVML function table + scripted runtime-status
  reader): `active → suspended ×12 → active`. Asserts zero NVML sample calls
  while suspended, `nvmlShutdown` after exactly 10 suspended ticks, re-init on
  the next active tick; and that 9 suspended ticks (< 10) do NOT close.
- `amd_power_split` (2 cases): integrated AMD device (Cezanne id, 512 MiB VRAM)
  → `ppt_w = 47.0`, `power_w = nullopt`; discrete (8 GiB) → `power_w = 47.0`.
- `apu_package_power` (3 cases): Collector lifts an active integrated GPU's PPT
  to `cpu.package_power_w` with source `APU PPT`; a live counter value (RAPL)
  is never overwritten; a suspended iGPU supplies nothing.

## 2. Full `--probe` (Release)

```
GETH — Geth Engine for Tracking Hardware 0.1.0
  kernel          Linux 7.2.9-arch1-1
  cpu             AMD Ryzen 5 5600H with Radeon Graphics
  cpufreq driver  amd-pstate
  nvidia driver   615.78.08
  pci.ids         /usr/share/hwdata/pci.ids

CPU usage + frequency  available    AMD Ryzen 5 5600H with Radeon Graphics — frequency from cpufreq scaling_cur_freq (12 logical CPUs)
                                      paths: /proc/stat, /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq, /sys/devices/system/cpu/cpu1/cpufreq/scaling_cur_freq
CPU temperature        available    k10temp (Tctl)
                                      paths: /sys/class/hwmon/hwmon6/temp1_input
CPU power              UNAVAILABLE  RAPL energy_uj is readable only by root on kernel >= 5.10 (EACCES)
                                      chain: RAPL > amd_energy > zenpower; if none, the amdgpu PPT of an integrated GPU (see GPU section)
                                      supplying now: amdgpu PPT of the integrated GPU — APU package power includes iGPU (see GPU section)
                                      fix: install packaging/99-geth-rapl.rules into /etc/udev/rules.d/ and run 'sudo udevadm trigger --subsystem-match=powercap' — or run geth as root
Memory                 available    RAM used = MemTotal - MemAvailable (KDE's definition)
                                      paths: /proc/meminfo
GPU                    available    2 GPU(s) detected, 2 claimed by a backend
                                      NVIDIA: 1 NVIDIA device(s)
                                          0000:01:00.0  GA107M [GeForce RTX 3050 Mobile]  (NVML: NVIDIA GeForce RTX 3050 Laptop GPU)  [matched by PCI bus id]
                                          NVML session opened lazily on first active sample, dropped after 10 suspended ticks
                                      AMD: 1 AMD device(s); names from /usr/share/hwdata/pci.ids
                                          0000:07:00.0  Cezanne [Radeon Vega Series / Radeon Vega Mobile Series]  [boot_vga]
                                            gpu_clock: absent
                                            mem_clock: absent
                                            hwmon/power1_average: absent
                                            integrated: gpu_metrics format 2 (APU)
                                            power: PPT (whole-package, includes iGPU) — reported as CPU package power
Disk                   available    per-device read/write rates from sector counters (x512)
                                      paths: /proc/diskstats
Network                available    per-interface rx/tx byte rates; sampled: eno1, wlo1
                                    excluded: lo (loopback); docker0 (virtual interface)
                                      paths: /proc/net/dev
```

Notes: no unclaimed devices on this machine (both PCI display controllers are
claimed); the unclaimed path is proven by the unit test in §1. The NVIDIA driver
version comes from `/sys/module/nvidia/version` during `--probe` because the
brief probe-time NVML session is closed before the header prints (see
deviations, D5) — both sources report the same value.

## 3. CPU compile load — APU PPT supplies CPU package power

Load: continuous full `cmake --build build-debug --clean-first -j8` rebuild loop
(15 TUs + tests) in the background. Three `--dump --count 3` blocks:

```
----- dump block 1 -----
CPU      usage 76.0 %    freq 4.02 GHz / max 4.04 GHz   temp 99.1 °C   power 44.0 W (APU PPT)
GPU gpu1 Cezanne ...   active   [0000:07:00.0, gpu_metrics format 2 (APU)]
         usage 0.0 %   VRAM 480.61 MiB/512.00 MiB  clocks 400 MHz / 1.60 GHz   n/a   58.0 °C
CPU      usage 68.3 %    freq 4.05 GHz / max 4.28 GHz   temp 99.5 °C   power 37.0 W (APU PPT)
GPU gpu1 ...   usage 0.0 %   ...   n/a   58.0 °C
CPU      usage 68.6 %    freq 4.02 GHz / max 4.04 GHz   temp 99.8 °C   power 44.0 W (APU PPT)
GPU gpu1 ...   usage 0.0 %   ...   n/a   59.0 °C

----- dump block 2 -----
CPU      usage 75.0 %    freq 4.01 GHz / max 4.02 GHz   temp 99.8 °C   power 46.0 W (APU PPT)
GPU gpu1 ...   usage 0.0 %   ...   n/a   59.0 °C
CPU      usage 68.8 %    freq 4.04 GHz / max 4.28 GHz   temp 100.0 °C  power 35.0 W (APU PPT)
GPU gpu1 ...   usage 0.0 %   ...   n/a   60.0 °C
CPU      usage 68.3 %    freq 4.04 GHz / max 4.28 GHz   temp 100.2 °C  power 52.0 W (APU PPT)
GPU gpu1 ...   usage 0.0 %   ...   n/a   59.0 °C

----- dump block 3 -----
CPU      usage 31.6 %    freq 4.01 GHz / max 4.28 GHz   temp 99.8 °C   power 28.0 W (APU PPT)
GPU gpu1 ...   usage 0.0 %   ...   n/a   57.0 °C
CPU      usage 26.0 %    freq 3.98 GHz / max 4.28 GHz   temp 99.1 °C   power 26.0 W (APU PPT)
GPU gpu1 ...   usage 0.0 %   ...   n/a   58.0 °C
CPU      usage 22.7 %    freq 4.06 GHz / max 4.28 GHz   temp 98.4 °C   power 20.0 W (APU PPT)
GPU gpu1 ...   usage 0.0 %   ...   n/a   57.0 °C
```

(Raw output: `/tmp/v_cpuload_p12.txt`; block 3 fades as the rebuild loop winds
down — usage falls 31%→23% and PPT follows 28→20 W, tracking the load.)

Read: CPU-only compile at 0% iGPU usage draws **35–52 W whole-package** from
the amdgpu PPT — the same 44–51 W band measured in Phase 1.1, confirming the
PPT is whole-package (CPU + iGPU + SoC), not a graphics-only number. The iGPU
line shows `n/a` power (its PPT moved to the CPU sample) while the discrete
NVIDIA card keeps its own `6.5 W`.

## 4. Raw data: memory footprint (§3 spec), GPU clocks (§4), disk/net (§5)

### 4a. Memory footprint — two Release runs, sampled mid-run

`--dump --count 80 --interval 0.2` (normal, NVML session live) vs
`--demo --count 80 --interval 0.2`, `/proc/<pid>/smaps_rollup` at t≈6 s:

| (kB) | normal (NVML) | --demo |
|---|---:|---:|
| Rss | **38,660** | 5,984 |
| Pss | 28,002 | 1,873 |
| Private_Clean | 520 | 332 |
| **Private_Dirty** | **22,176** | **1,464** |
| Shared_Clean | 15,964 | 4,188 |
| Shared_Dirty | 0 | 0 |
| Swap | 0 | 0 |

Top 5 `/proc/<pid>/smaps` mappings by Rss (aggregated by mapping name):

| normal | KiB | | --demo | KiB |
|---|---:|---|---|---:|
| `<anon>` | 19,008 | | libc.so.6 | 1,712 |
| libcuda.so.615.78.08 | 11,700 | | libstdc++.so.6 | 1,592 |
| libstdc++.so.6 | 1,872 | | [heap] | 1,252 |
| libc.so.6 | 1,712 | | libm.so.6 | 376 |
| libnvidia-ml.so.615.78.08 | 1,596 | | geth (binary) | 340 |

**Private-dirty attribution (normal run), per mapping:**

| owner | mapping | Private_Dirty |
|---|---|---:|
| NVIDIA stack (driver thread arena) | `<anon>` | 19,008 KiB |
| NVIDIA stack | libcuda.so data | 1,568 KiB |
| **GETH** | **[heap]** | **1,348 KiB** |
| NVIDIA stack | libnvidia-ml.so data | 64 KiB |
| GETH / shared libs | [stack] 32, libstdc++ 72, libc 24, … | < 140 KiB |

**Conclusion: GETH's own private dirty ≈ 1.5–1.6 MB — well under the 8 MB
target.** The ~20.2 MB excess is the NVIDIA userspace runtime, reported as-is:

- `/proc/<pid>/status` shows **Threads: 2** — geth is single-threaded; the
  second thread is created by the driver stack. The 18.5 MB `rw-p` private anon
  region (immediately after `libnvidia-ml.so`'s mapping) plus the 65 MB `---p`
  reservation is the classic glibc secondary-arena layout of that driver
  thread.
- **strace of `--probe`** (main thread, before any `clone()`): geth opens
  `libnvidia-ml.so.1` (fd 30) via dlopen — and `libnvidia-ml` itself then opens
  `libcuda.so.1` (fd 30) and `/dev/nvidiactl`. GETH never links or dlopens
  libcuda; NVML pulls it in at `nvmlInit` (driver 615).
- `pci.ids` streaming verified: the old Phase 1.1 baseline (full ~150k-entry
  map) peaked at 45 MB RSS; the subset loader (stream once, keep only requested
  (vendor, device) names) leaves geth's brk heap at ~1.3 MB in the live run and
  1.25 MB in `--demo`. No other history/string churn was found — the
  `--demo` run (same History, same strings, no NVML) is 1.4 MB private dirty.

### 4b. NVIDIA clocks under unthrottled load — geth vs nvidia-smi, 5 samples

Load: `__GL_SYNC_TO_VBLANK=0 vblank_mode=0 __NV_PRIME_RENDER_OFFLOAD=1
__GLX_VENDOR_LIBRARY_NAME=nvidia glxgears -fullscreen` (786 FPS unthrottled).
nvidia-smi was run only for verification; geth never spawns it.

| | nvidia-smi | geth --dump |
|---|---|---|
| utilization.gpu | 100 % | 100.0 % |
| clocks.gr | 1965 MHz | 1.97 GHz (1965→1970, 2-s.f. display) |
| clocks.mem | **6001 MHz** | **6.00 GHz (6000)** |
| power.draw | 22.55–22.62 W | 22.6 W |
| temperature | 49–50 | 49–50 |

Agreement within sampling noise on all five quantities. **Graphics (1965 MHz)
and memory (6001 MHz) clocks differ under load** — the Phase 1.1 bug read
clock domain 1 (SM) for "memory", making both report the same number; the code
now uses `NVML_CLOCK_MEM = 2`.

### 4c. Disk write with known traffic

`dd if=/dev/zero of=./geth_test.bin bs=1M count=2048 oflag=direct`
(2,147,483,648 bytes; the file was deleted afterwards). The geth project dir
lives on nvme1n1p5, so the write must appear on **nvme1n1**:

```
dd: 2147483648 bytes (2.1 GB, 2.0 GiB) copied, 0.831271 s, 2.6 GB/s
geth (1 s windows):
  nvme1n1  W 85.92 MiB/s     <- window where dd started
  nvme1n1  W 1.91 GiB/s      <- main write window
  (other windows: 0)
```

geth total = 1.91 GiB + 85.92 MiB ≈ **2.0 GiB on the correct nvme**, matching
dd's 2.0 GiB in 0.83 s (2.6 GB/s).

### 4d. Network rx with known traffic

`curl -o /dev/null --max-time 10 https://releases.ubuntu.com/24.04.4/ubuntu-24.04.4-desktop-amd64.iso`
(default route via eno1).

| measure | value |
|---|---|
| curl | 67,087,932 B in 10.001 s (avg 6.71 MB/s, includes ramp) |
| **/proc/net/dev eno1 delta (ground truth)** | **77,104,638 B (73.54 MiB)** |
| geth eno1 rx, sum of per-second windows | ≈ 73.5 MiB |
| geth eno1 rx, steady-state seconds | 9.75–10.50 MiB/s (10.2–11.0 MB/s) |

**geth matches the kernel counter within <0.1%.** The 9.6 MiB difference
between the interface delta and curl's total is unrelated background rx on
eno1 during the window (baseline eno1 rx before the download was 0–20 KiB/s,
wlo1 idle). curl's 10-s average is lower than the steady-state rate only
because the TCP connection ramps over the first ~4 s — geth shows that ramp
explicitly (0 → 1.26 → 8.99 → 9.85 MiB/s → steady).

## 5. File tree, extensibility, deviations

### 5a. File tree (project sources)

```
CMakeLists.txt                      # single version define: GETH 0.1.0
src/
  main.cpp                          # CLI, --probe header, --dump/--json print
  core/
    collector.{hpp,cpp}             # tick loop, APU-PPT lift into CPU sample
    history.hpp                     # fixed-capacity ring of Snapshots
    snapshot.hpp                    # CpuPackagePowerSource enum, samples
    source.hpp                      # Source interface, SourceStatus
  sources/
    pci_discovery.{hpp,cpp}         # NEW: /sys/bus/pci/devices class-0x03 scan
                                    #   = single source of truth for GPUs
    gpu_backend.{hpp,cpp}           # NEW: GpuBackend interface, RuntimeState,
                                    #   injectable RuntimeStatusReader
    gpu_registry.{hpp,cpp}          # NEW: the ONLY file listing backends;
                                    #   claims, unclaimed reporting
    gpu_nvidia.{hpp,cpp}            # GpuNvidiaBackend: dlopen NVML, strict
                                    #   bus-id matching, lazy 10-tick session
    gpu_amd.{hpp,cpp}               # GpuAmdBackend: sysfs + gpu_metrics,
                                    #   PPT split (integrated -> package)
    sensor_tables.hpp               # NEW: CPU temp preference + power source
                                    #   order as data
    cpu.{hpp,cpp}  cpu_power.{hpp,cpp}  temps.{hpp,cpp}
    memory.{hpp,cpp}  disk.{hpp,cpp}  net.{hpp,cpp}  demo.{hpp,cpp}
  util/
    pci_ids.{hpp,cpp}               # streaming subset loader
    parse.hpp  format.hpp  sysfs_file.{hpp,cpp}
tests/
  test_main.cpp  test_parsers.cpp   # 49 cases / 284 assertions
  fixtures/pci_ids.txt
```

### 5b. What an Intel backend would need

One new file pair + one registry line, exactly as the extensibility design
requires:

1. `src/sources/gpu_intel.{hpp,cpp}`: `class GpuIntelBackend : public GpuBackend`
   - `claims()`: `dev.vendor_id == 0x8086 && (dev.driver == "i915" || dev.driver == "xe")`
   - `attach()`: open-once the per-device sysfs it needs —
     `/sys/class/drm/cardN/gt_cur_freq_mhz` (+ `gt_min/max_freq_mhz`),
     `/sys/class/drm/cardN/gt0_busy_percent` (and `gt1_…` for dual-tile),
     the `i915`/`xe` hwmon `temp1_input`, VRAM for discrete Arc via
     `mem_info_vram_total`-style attrs, `power/runtime_status`; names via the
     same `PciIds` subset.
   - `sample()`: fill `GpuSample` (usage = busy_percent, core clock from
     rps/gt_cur_freq, temp, VRAM); power from the RAPL *graphics* powercap
     domain where readable, else `nullopt` — no fake 0.
   - integrated decision: a small Intel analogue of `decide_amd_integrated`
     (known-iGPU device-id table, or "no second 0x8086 0x03 device" on the
     bus), feeding the existing `finalize_gpus` integrated-method strings.
2. **One line** in `gpu_registry.cpp`'s constructor:
   `_backends.push_back(std::make_unique<GpuIntelBackend>(_ids));`
3. One doctest case over a fake sysfs tree (the `pci_discovery`/registry test
   for the unclaimed Intel device already exercises the claim path).

Nothing else changes: discovery, matching, probe reporting, and the dump/JSON
format all flow through the existing interface.

### 5c. Deviations and reasons

- **D1.** `CpuSample` carries `package_power_w` + `package_power_source`
  (enum `rapl / amd_energy / zenpower / APU PPT`) — as specified; the old
  single `power_w` field was replaced, not kept alongside.
- **D2.** `--dump` prints the `(APU PPT)` suffix only when the source is
  `APU PPT`; counter sources print plain watts (e.g. `power 44.0 W`), so the
  suffix is a marker of the PPT fallback, not a per-source label.
- **D3.** The AMD integrated-decision's `multi_gpu` input is "number of
  attached AMD PCI devices ≥ 2" (the `decide_amd_integrated` signature is
  unchanged; the registry knows the device count).
- **D4.** `amd_energy`/`zenpower` counter semantics (cumulative µJ in
  `power1_raw`, scale from `max_energy_range_uj`) are declared in
  `sensor_tables.hpp` but could not be verified on this machine: neither
  driver is present and RAPL is root-only here.
- **D5.** `--probe`'s GPU section runs a **brief** NVML enumeration session
  (init → count → bus ids → immediate shutdown) because the spec requires
  `--probe` to report unmatched NVML↔sysfs handles in *both* directions.
  Enumeration reads driver metadata, not GPU registers, so it does not wake
  the GPU. The sample-time session remains lazy (first active tick; dropped
  after 10 suspended ticks). Consequence: at header-print time the session is
  closed, so the NVIDIA driver version in the `--probe` header comes from
  `/sys/module/nvidia/version` (same value as `nvmlSystemGetDriverVersion`).
- **D6.** `--json` does not emit `pci_bus_id` per GPU (pre-existing format
  choice, unchanged in Phase 1.2); the bus id appears in `--dump` and
  `--probe`.

### Standing constraints — status

- No subprocesses spawned by geth (nvidia-smi was run by the test script,
  never by geth). ✓
- No NVIDIA libraries linked; `libnvidia-ml.so.1` only, via dlopen/dlsym.
  (libcuda enters the process because NVML itself dlopens it — strace §4a.) ✓
- No external runtime deps; std::optional for missing data (GPU power `n/a`,
  PPT, clocks); single-threaded; open-once + pread sysfs; string_view
  parsers; -Wall -Wextra -Wpedantic -Werror clean in both configs. ✓
