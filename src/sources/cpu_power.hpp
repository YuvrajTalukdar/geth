// CPU package power. The preference order is DATA in sensor_tables.hpp:
//   1. RAPL (powercap "intel-rapl:N" — AMD Zen CPUs also appear under this
//      name): cumulative energy_uj counter, power = Δenergy / Δtime.
//   2. AMD hwmon drivers: "amd_energy", then "zenpower" — both expose a
//      cumulative package energy counter (microjoules) in power1_raw.
//   3. (Collector) amdgpu PPT of an integrated GPU — on AMD APUs that
//      reading is whole-package power (CPU + iGPU + SoC), so it is the CPU's
//      package power when no counter source exists.
//
// energy_uj has been root-readable since kernel 5.10, so probe() must report
// EACCES with a fix hint (see packaging/99-geth-rapl.rules).
#pragma once

#include "core/source.hpp"
#include "util/sysfs_file.hpp"

#include <optional>
#include <string>
#include <vector>

namespace geth {

// Delta of a cumulative energy counter that may have wrapped around.
// `max_range` is the counter's full scale (max_energy_range_uj); pass 0 when
// the driver publishes no range, in which case a plain mod-2^64 difference is
// used — 2^64 microjoules is ~31,700 years at 1 W, so that is safe.
std::optional<unsigned long long> counter_delta(unsigned long long now,
                                                unsigned long long prev,
                                                unsigned long long max_range);

class CpuPowerSource : public Source {
 public:
  const char* name() const override { return "CPU power"; }
  SourceStatus probe() override;
  void sample(Snapshot& out) override;

 private:
  struct Domain {
    SysfsFile energy;  // cumulative counter in microjoules
    SysfsFile range;   // full counter scale, if the driver exposes one
    unsigned long long prev_uj = 0;
  };
  std::vector<Domain> _domains;
  TimePoint _prev_tp;
  bool _have_prev_tp = false;
  // Which counter source won (empty = none: the Collector may fall back to
  // the integrated GPU's APU PPT).
  std::optional<CpuPackagePowerSource> _backend;
};

}  // namespace geth
