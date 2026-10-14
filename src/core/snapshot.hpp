// One sample of every metric GETH collects.
//
// Every field that may be unavailable on a given machine is std::optional —
// GETH never invents a fake 0 for missing data; the UI renders "n/a" instead.
#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace geth {

// steady_clock is CLOCK_MONOTONIC on Linux; it does not jump when wall-clock
// time changes, which makes it the right base for rate calculations.
using Clock = std::chrono::steady_clock;
using TimePoint = Clock::time_point;

// Where the CPU package power figure came from. Counter-based sources are
// probed in this order (see sensor_tables.hpp); kApuPpt is injected by the
// Collector only when no counter source exists: on AMD APUs the amdgpu PPT
// reading is whole-package power (CPU + iGPU + SoC), so it is CPU power
// *only* while an integrated GPU is awake.
enum class CpuPackagePowerSource { kRapl, kAmdEnergy, kZenpower, kApuPpt };

constexpr const char* to_string(CpuPackagePowerSource s) {
  switch (s) {
    case CpuPackagePowerSource::kRapl: return "RAPL";
    case CpuPackagePowerSource::kAmdEnergy: return "amd_energy";
    case CpuPackagePowerSource::kZenpower: return "zenpower";
    case CpuPackagePowerSource::kApuPpt: return "APU PPT";
  }
  return "unknown";
}

struct CpuSample {
  std::optional<double> usage_pct;
  std::vector<std::optional<double>> per_core_pct;
  std::optional<double> freq_avg_mhz;
  std::optional<double> freq_max_mhz;
  std::optional<double> temp_c;
  // Whole-package power (CPU, and on APUs the SoC/iGPU as well).
  std::optional<double> package_power_w;
  std::optional<CpuPackagePowerSource> package_power_source;
};

struct MemSample {
  std::optional<double> ram_total_bytes;
  std::optional<double> ram_used_bytes;
  std::optional<double> swap_total_bytes;
  std::optional<double> swap_used_bytes;
};

enum class GpuVendor { kNvidia, kAmd };
enum class GpuState { kActive, kSleeping, kError };

struct GpuSample {
  std::string id;  // "gpu0", "gpu1", ... — stable, assigned in PCI bus order
  std::string name;
  GpuVendor vendor = GpuVendor::kAmd;
  std::string pci_bus_id;  // "0000:01:00.0"
  bool is_integrated = false;
  // How is_integrated was decided: "gpu_metrics (format 2)", "APU device id
  // 0x1638", "VRAM <= 2 GiB", "boot_vga tiebreaker", "always discrete" ...
  // The UI/probe print it so an "integrated" flag is never a mystery.
  std::string integrated_method;
  GpuState state = GpuState::kActive;
  std::optional<double> usage_pct;
  std::optional<double> vram_used_bytes;
  std::optional<double> vram_total_bytes;
  std::optional<double> core_clock_mhz;
  std::optional<double> mem_clock_mhz;
  // Power of the card itself. On integrated AMD GPUs this is nullopt: the
  // amdgpu PPT reading measures the whole package (CPU included), so it is
  // reported as CPU package power instead. Discrete GPUs fill this.
  std::optional<double> power_w;
  // Raw amdgpu hwmon PPT reading (microwatts-based), always kept when the
  // driver exposes it — for --probe/debugging. Never shown as a metric.
  std::optional<double> ppt_w;
  std::optional<double> temp_c;
  // Owns the BIOS framebuffer. Extra (beyond the base data model) because it
  // feeds the is_integrated heuristic and is useful to the UI later.
  bool boot_vga = false;
};

struct DiskDeviceSample {
  std::string name;
  std::optional<double> read_bytes_per_s;
  std::optional<double> write_bytes_per_s;
};

struct DiskSample {
  std::vector<DiskDeviceSample> devices;
  std::optional<double> total_read_bytes_per_s;
  std::optional<double> total_write_bytes_per_s;
};

struct NetIfSample {
  std::string name;
  std::optional<double> rx_bytes_per_s;
  std::optional<double> tx_bytes_per_s;
};

struct NetSample {
  std::vector<NetIfSample> interfaces;
  std::optional<double> total_rx_bytes_per_s;
  std::optional<double> total_tx_bytes_per_s;
};

struct Snapshot {
  TimePoint time;
  CpuSample cpu;
  MemSample mem;
  std::vector<GpuSample> gpus;
  DiskSample disk;
  NetSample net;
};

constexpr const char* to_string(GpuVendor v) {
  return v == GpuVendor::kNvidia ? "nvidia" : "amd";
}

constexpr const char* to_string(GpuState s) {
  switch (s) {
    case GpuState::kActive: return "active";
    case GpuState::kSleeping: return "sleeping";
    case GpuState::kError: return "error";
  }
  return "unknown";
}

}  // namespace geth
