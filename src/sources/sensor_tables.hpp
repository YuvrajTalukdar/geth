// Sensor preference tables — data, not code.
//
// Both tables express *order*: which hwmon sensor label to trust first, and
// which CPU package power source to prefer. They live here instead of as
// if/else chains inside the sources, so adding a driver or a power source is
// a table edit, not a logic change.
#pragma once

#include "core/snapshot.hpp"  // CpuPackagePowerSource

#include <string>
#include <vector>

namespace geth {

// ---------------------------------------------------------------------------
// CPU temperature: per-driver ordered label preference.
//
// For a hwmon device whose `name` file is `driver`, trust the first
// tempN_label in `preferred_labels` that exists. `accept_any` (acpitz) means
// the driver's sensors are unlabelled garbage-free ACPI readings: take the
// first. `kThermalZone` entries are not hwmon devices at all — probe()
// special-cases them (x86_pkg_temp is a /sys/class/thermal zone, not a hwmon
// `name`).
// ---------------------------------------------------------------------------

struct TempPreference {
  enum class Kind { kHwmon, kThermalZone };
  const char* driver;                  // hwmon `name`, or the zone type
  std::vector<std::string> preferred_labels;
  Kind kind = Kind::kHwmon;
  bool accept_any = false;
};

// Order is the preference order: first entry that has a readable sensor wins.
inline const std::vector<TempPreference>& cpu_temp_preferences() {
  static const std::vector<TempPreference> table = {
      // AMD: Tdie is the real die; many APUs expose only Tctl — that is fine.
      {"k10temp", {"Tdie", "Tctl"}},
      {"zenpower", {"Tdie"}},
      // Intel: the package sensor.
      {"coretemp", {"Package id 0"}},
      // The kernel's own package sensor, when exposed as a thermal zone.
      {"x86_pkg_temp", {}, TempPreference::Kind::kThermalZone, false},
      // ACPI: last resort, unlabelled.
      {"acpitz", {}, TempPreference::Kind::kHwmon, true},
  };
  return table;
}

// ---------------------------------------------------------------------------
// CPU package power: ordered source preference.
//
// The counter-based sources (rapl, amd_energy, zenpower) are probed by
// CpuPowerSource itself, in this order; the first readable one wins.
// amd_energy was removed from mainline kernels but is still present in some
// distro kernels, so it stays in the table; zenpower is an out-of-tree
// module (its presence is fine, but --probe never suggests loading it).
// kApuPpt is NOT probed there: on AMD APUs the amdgpu hwmon PPT reading is
// the *whole-package* power (CPU + iGPU + SoC), so it can only be used once
// the Collector knows an integrated GPU exists — the Collector injects it as
// the fourth, lowest-priority source at sample time.
// ---------------------------------------------------------------------------

inline const std::vector<CpuPackagePowerSource>& cpu_power_counter_sources() {
  static const std::vector<CpuPackagePowerSource> order = {
      CpuPackagePowerSource::kRapl,
      CpuPackagePowerSource::kAmdEnergy,
      CpuPackagePowerSource::kZenpower,
  };
  return order;
}

}  // namespace geth
