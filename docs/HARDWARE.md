# GETH — Hardware support

All metrics are read in-process (no `lshw`, `sensors`, `nvml` CLI, etc.).
`--probe` lists every source on the current machine, whether it is available,
and why not — with fix hints.

| Component | Source | Notes |
|---|---|---|
| CPU usage + frequency | `/proc/stat`, cpufreq `scaling_cur_freq` | AMD Zen (tested: Ryzen 5 5600H). Logical-CPU count from `/proc/stat`. |
| CPU temperature | `hwmon` (`k10temp` on AMD) | First usable `temp*_input`. |
| CPU package power | RAPL → `amd_energy` → `zenpower` → APU PPT fallback | See [CPU power on AMD](#cpu-power-on-amd). On an AMD APU with RAPL unreadable, the integrated GPU's **PPT** supplies package power (it already includes the iGPU). |
| RAM / swap | `/proc/meminfo` | RAM used = `MemTotal - MemAvailable` (KDE's definition). |
| Integrated GPU (AMD) | `amdgpu` sysfs (`gpu_busy_percent`, `mem_info_vram_used`, GTT, clocks, PPT) | Detected by PCI id; APU format via `gpu_metrics`. |
| Discrete GPU (NVIDIA) | NVML (loaded via `dlopen`, never linked) | Usage, memory, power, clocks, temperature, fan. Session opens lazily and drops after 10 suspended ticks (see [Hybrid laptops](#hybrid-laptops)). |
| Disk | `/proc/diskstats` | Per-device read/write byte rates (sector counters × 512). |
| Network | `/proc/net/dev` | Per-interface rx/tx byte rates; `lo` and virtual interfaces excluded. |

## Tested / expected machines

**Tested** — the dev machine this project is developed and tested on:

| Machine | CPU | iGPU | dGPU | Status |
|---|---|---|---|---|
| Hybrid laptop | AMD Ryzen 5 5600H (APU) | Radeon Vega 7 (Cezanne) | NVIDIA GeForce RTX 3050 Laptop | **Tested** — RAPL is present but unreadable (EACCES), so APU PPT supplies CPU package power |

**Expected to work** (same interfaces; not personally tested yet):

- AMD Ryzen 7 260 + Radeon 780M + NVIDIA GeForce RTX 5060 Laptop (hybrid)
- AMD Ryzen 7 5800X + NVIDIA GeForce RTX 3070 (desktop, no iGPU)
- Other AMD and Intel CPUs with standard `/proc` + `hwmon` interfaces
- NVIDIA GPUs under the proprietary driver (NVML)
- AMD GPUs under `amdgpu` (including integrated iGPUs)

Notes:

- **Intel iGPUs are detected but not sampled** — the PCI device is listed in
  `--probe` and the panel skeleton appears, but there is no Intel GPU
  backend yet, so the panel shows "—".
- GETH needs **no root** for any of this; the only optional root step is
  making RAPL readable without a root account (below).

## CPU power on AMD

CPU package power comes from a chain of sources, in order: **RAPL** (the
`powercap` `energy_uj` counters), then the `amd_energy` hwmon, then the
`zenpower` hwmon, and finally — on an AMD APU — the **integrated GPU's
PPT**, whose APU package power already includes the iGPU, so the
"APU Package (PPT)" line stays correct.

RAPL **exists on AMD Zen** too — it is the same `powercap`
`intel_rapl_common` interface. On kernel ≥ 5.10 the `energy_uj` files are
`0400 root:root`, so without extra permissions GETH reads **EACCES**
(unreadable) — *not* "absent". `--probe` reports EACCES, the binary's
effective capabilities (`CapEff`; `cap_dac_read_search` is bit 2, so a failed
or stale `setcap` is visible), and which source is supplying CPU package
power in the meantime. Until a fix is applied on an APU, the panel shows the
APU PPT value, which is a correct package-power measurement.

Two optional fixes, one command each (run from a build directory; each needs
`sudo` and is run by you, not by GETH):

```sh
sudo cmake --build build --target install-udev  # udev rule for energy_uj
sudo cmake --build build --target setcap        # cap_dac_read_search on the binary
```

From an installed package, the equivalent of `install-udev` is:

```sh
sudo cp /usr/share/geth/99-geth-rapl.rules /etc/udev/rules.d/
sudo udevadm trigger --subsystem-match=powercap
```

`install-udev` is the more surgical option: it installs
`packaging/99-geth-rapl.rules`, which makes only the `package-*` powercap
domains world-readable. `setcap` grants `cap_dac_read_search` on the binary,
which bypasses file permission checks more broadly — prefer the udev rule on
shared machines. As a last resort, run `geth` as root.

Verify after enabling:

```sh
ls -l /sys/class/powercap/intel-rapl:*/energy_uj   # should be 0644
geth --probe                                        # "CPU power … available (RAPL …)"
```

`--probe` also cross-checks RAPL against the APU PPT when both are readable
(the two are independent measurements of nearly the same quantity and should
agree within a few watts under load).

## Hybrid laptops

On Optimus/hybrid laptops the discrete NVIDIA GPU is powered down when idle.
GETH handles this without any user action:

- NVML is **never linked**; it is `dlopen`ed on first use, so a machine with
  no NVIDIA driver (or no NVML) still runs.
- The NVML session **opens lazily** on the first tick where a card is not
  suspended, and is **dropped after 10 consecutive suspended ticks** (the
  card is back to sleep). Waking the card is what costs power, so GETH does
  not poll a sleeping card — it never wakes a sleeping dGPU.
- A suspended card shows its last known values (dimmed) rather than zeroes,
  so the panel does not flicker to 0% while the dGPU is asleep.
- NVML enumeration happens only in `--probe` mode, never at UI startup.

## Hotplugging

The set of devices GETH samples is fixed at **probe time** (the first tick of
the run): which network interfaces pass the filter, which whole disks exist,
and which GPUs are present. GETH does not re-enumerate devices at runtime —
the per-tick path stays allocation-free and the graph's history lines stay
aligned. So:

- **A device that appears after startup is not sampled until the next geth
  run** (a network interface coming up, a USB disk plugged in, a dGPU
  detected). This is a documented limitation, not a bug.
- **A device that disappears while GETH is running is safe.** It never
  crashes GETH and never repeats a stale value. On the next tick, an
  interface or disk that is no longer in the kernel's counters
  (`/proc/net/dev`, `/proc/diskstats`) is reported as *absent* — not zero,
  not the previous value. Absent is drawn as a **gap** in the graph (a NaN in
  the history ring) and an **empty cell** in a CSV log. The surviving devices
  keep sampling normally.
- **A GPU that stops reporting** (a sysfs-backed amdgpu whose files vanish, a
  powered-off card) likewise drops to a gap; a *suspended* NVIDIA dGPU keeps
  its last known values dimmed instead (see [Hybrid laptops](#hybrid-laptops)).

"A device is gone" and "a device is idle" are different, and GETH keeps them
apart: a removed device is a **gap**, a present-but-idle device is a
(correct) **0**.
