// CPU package power. The preference order is DATA in sensor_tables.hpp
// (cpu_power_counter_sources()):
//   1. RAPL (powercap "intel-rapl:N" — AMD Zen CPUs also appear under this
//      name): cumulative package energy in energy_uj (microjoules), power =
//      Δenergy / Δtime. The counter wraps at max_energy_range_uj.
//   2. AMD hwmon "amd_energy" (drivers/hwmon/amd_energy.c; documented in
//      Documentation/hwmon/amd_energy.rst in the kernel tree): one cumulative
//      energy counter per entry — energyN_input, in microjoules — named by
//      energyN_label: "EcoreNNN" (per core) or "EsocketN" (per socket). Only
//      the Esocket* entries are package power, so only those are summed. The
//      driver keeps a 64-bit accumulator and publishes no range, so a
//      backwards step is a driver RESET, not a wraparound: that sample is
//      skipped and the baseline re-anchored.
//      (amd_energy was removed from mainline kernels but is still in some
//      distro kernels, so it stays in the table.)
//   3. AMD hwmon "zenpower" (an OUT-OF-TREE module; layout per its README):
//      two INSTANTANEOUS power inputs in microwatts —
//        power1_input  (label SVI2_P_Core)  — the CPU cores
//        power2_input  (label SVI2_P_SoC)   — the SoC
//      Package power is the sum; there is no counter math. Do not suggest
//      loading it from --probe: it is out-of-tree and requires k10temp to be
//      unloaded first.
//   4. (Collector) amdgpu PPT of an integrated GPU — on AMD APUs that reading
//      is whole-package power (CPU + iGPU + SoC), so it is the CPU's package
//      power when no counter source exists.
//
// RAPL energy_uj has been root-readable since kernel 5.10, so probe() must
// report EACCES with a fix hint (see packaging/99-geth-rapl.rules).
#pragma once

#include "core/source.hpp"
#include "util/sysfs_file.hpp"

#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace geth {

// Delta of a cumulative energy counter that may have wrapped around.
// `max_range` is the counter's full scale (RAPL's max_energy_range_uj). Pass
// 0 when the driver publishes no range (amd_energy): a backwards step then
// means a reset, not a wrap, and nullopt is returned.
std::optional<unsigned long long> counter_delta(unsigned long long now,
                                                unsigned long long prev,
                                                unsigned long long max_range);

class CpuPowerSource : public Source {
 public:
  // `root` is the sysfs tree root ("/sys" in production; a fixture directory
  // in tests — the probe reads <root>/class/powercap and <root>/class/hwmon).
  explicit CpuPowerSource(std::string root = "/sys") : _root(std::move(root)) {}

  const char* name() const override { return "CPU power"; }
  SourceStatus probe() override;
  void sample(Snapshot& out) override;

 private:
  struct Domain {
    SysfsFile energy;  // cumulative counter in microjoules
    SysfsFile range;   // full counter scale (RAPL only; invalid = no wrap)
    unsigned long long prev_uj = 0;
  };

  // True when a usable instance of the named driver was found and opened.
  bool probe_amd_energy(SourceStatus& st, const std::string& hwmon,
                        std::error_code& ec);
  bool probe_zenpower(SourceStatus& st, const std::string& hwmon,
                      std::error_code& ec);

  std::string _root;
  std::vector<Domain> _domains;  // RAPL / amd_energy energy counters
  SysfsFile _z_core;             // zenpower power1_input (instantaneous µW)
  SysfsFile _z_soc;              // zenpower power2_input (instantaneous µW)
  TimePoint _prev_tp;
  bool _have_prev_tp = false;
  // Which counter source won (nullopt = none: the Collector may fall back to
  // the integrated GPU's APU PPT).
  std::optional<CpuPackagePowerSource> _backend;
};

}  // namespace geth
