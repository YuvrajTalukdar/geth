#include "sources/demo.hpp"

#include <algorithm>
#include <cmath>

namespace geth {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kGiB = 1024.0 * 1024.0 * 1024.0;
constexpr int kCores = 16;

double clamp01(double x) {
  return std::clamp(x, 0.0, 1.0);
}

// Smooth pseudo-wave: deterministic, so --demo output is reproducible.
double wave(double t, double period, double phase) {
  return std::sin(2.0 * kPi * (t / period + phase));
}

}  // namespace

SourceStatus DemoSource::probe() {
  SourceStatus st;
  st.description = name();
  st.available = true;
  st.detail = "synthetic data; no hardware is read";
  return st;
}

void DemoSource::sample(Snapshot& out) {
  const TimePoint now = Clock::now();
  if (!_started) {
    _t0 = now;
    _started = true;
  }
  const double t = std::chrono::duration<double>(now - _t0).count();
  out.time = now;

  // One-time sizing of the reused Snapshot's containers; the identity
  // strings below are written only on that first fill.
  if (out.cpu.per_core_pct.size() != kCores)
    out.cpu.per_core_pct.resize(kCores);
  if (out.gpus.size() != 2) {
    out.gpus.resize(2);
    GpuSample& n = out.gpus[0];
    n.vendor = GpuVendor::kNvidia;
    // Realistic pci.ids-style strings so the UI short-name logic produces the
    // spec examples ("RTX 3050 Laptop" / "Cezanne iGPU").
    n.name = "NVIDIA GeForce RTX 3050 Laptop GPU";
    n.pci_bus_id = "0000:01:00.0";
    n.boot_vga = false;
    n.is_integrated = false;
    n.integrated_method = "always discrete (no NVIDIA iGPUs)";
    n.vram_total_bytes = 8.0 * kGiB;
    GpuSample& a = out.gpus[1];
    a.vendor = GpuVendor::kAmd;
    a.name = "Cezanne [Radeon Vega Series / Radeon Vega Mobile Series]";
    a.pci_bus_id = "0000:07:00.0";
    a.boot_vga = true;
    a.is_integrated = true;
    // Same string the live source produces for a Cezanne iGPU.
    a.integrated_method = "gpu_metrics format 2 (APU)";
    a.vram_total_bytes = 1.0 * kGiB;
  }
  if (out.disk.devices.size() != 2) {
    out.disk.devices.resize(2);
    out.disk.devices[0].name = "nvme0n1";
    out.disk.devices[1].name = "sda";
  }
  if (out.net.interfaces.size() != 2) {
    out.net.interfaces.resize(2);
    out.net.interfaces[0].name = "eno1";
    out.net.interfaces[1].name = "wlo1";
  }

  // --- CPU ---------------------------------------------------------------
  out.cpu.usage_pct =
      100.0 * clamp01(0.40 + 0.25 * wave(t, 47, 0) + 0.06 * wave(t, 7.3, 0.3));
  for (int i = 0; i < kCores; ++i)
    out.cpu.per_core_pct[i] =
        100.0 * clamp01(0.35 + 0.20 * wave(t, 31, 0.13 * i));
  out.cpu.freq_avg_mhz = 1600.0 + 900.0 * wave(t, 53, 0.1);
  out.cpu.freq_max_mhz = *out.cpu.freq_avg_mhz + 400.0;
  out.cpu.temp_c = 48.0 + 9.0 * wave(t, 61, 0.2);
  out.cpu.package_power_w = 22.0 + 9.0 * wave(t, 41, 0.7);
  out.cpu.package_power_source = CpuPackagePowerSource::kApuPpt;  // demo is an APU

  // --- Memory ------------------------------------------------------------
  out.mem.ram_total_bytes = 32.0 * kGiB;
  out.mem.ram_used_bytes = (12.0 + 1.5 * wave(t, 97, 0.5)) * kGiB;
  out.mem.swap_total_bytes = 2.0 * kGiB;
  out.mem.swap_used_bytes = (0.5 + 0.4 * wave(t, 113, 0.2)) * kGiB;

  // --- GPUs (one per vendor, mirroring the dev machine: an RTX 3050 Laptop
  //     dGPU and a Cezanne Vega 7 iGPU). -----------------------------------
  GpuSample& nvidia = out.gpus[0];
  nvidia.reset();
  nvidia.usage_pct = 100.0 * clamp01(0.12 + 0.45 * std::abs(wave(t, 23, 0.4)));
  nvidia.vram_used_bytes = (5.5 + 1.0 * wave(t, 67, 0.1)) * kGiB;
  nvidia.core_clock_mhz = 1650.0 + 350.0 * wave(t, 13, 0.1);
  nvidia.mem_clock_mhz = 1750.0;
  nvidia.power_w = 28.0 + 14.0 * wave(t, 17, 0.6);
  nvidia.temp_c = 47.0 + 8.0 * wave(t, 29, 0.2);

  GpuSample& amd = out.gpus[1];
  amd.reset();
  amd.usage_pct = 100.0 * clamp01(0.04 + 0.12 * std::abs(wave(t, 31, 0.8)));
  amd.vram_used_bytes = (0.4 + 0.1 * wave(t, 43, 0.3)) * kGiB;
  amd.core_clock_mhz = 1100.0 + 200.0 * wave(t, 19, 0.5);
  amd.mem_clock_mhz = 1600.0;
  // Integrated: the PPT reading is whole-package power (CPU + iGPU), so it
  // is NOT this card's power — it is the CPU's package power (shown above).
  // The raw value is kept for --probe/debugging.
  amd.ppt_w = 5.0 + 2.0 * wave(t, 23, 0.9);
  amd.temp_c = 41.0 + 5.0 * wave(t, 37, 0.4);

  // --- Disk ---------------------------------------------------------------
  auto spike = [&](double period, double phase) {
    const double w = std::max(0.0, wave(t, period, phase));
    return w * w;  // occasional bursts instead of constant traffic
  };
  DiskDeviceSample& nvme = out.disk.devices[0];
  nvme.read_bytes_per_s = (4.0 + 30.0 * spike(19, 0.1)) * 1048576.0;
  nvme.write_bytes_per_s = (1.0 + 2.0 * std::abs(wave(t, 11, 0.4))) * 1048576.0;
  DiskDeviceSample& sda = out.disk.devices[1];
  sda.read_bytes_per_s = 0.5 * 1048576.0;
  sda.write_bytes_per_s = (2.5 + 1.5 * wave(t, 29, 0.6)) * 1048576.0;
  out.disk.total_read_bytes_per_s = *nvme.read_bytes_per_s + *sda.read_bytes_per_s;
  out.disk.total_write_bytes_per_s = *nvme.write_bytes_per_s + *sda.write_bytes_per_s;

  // --- Network -------------------------------------------------------------
  NetIfSample& eno1 = out.net.interfaces[0];
  eno1.rx_bytes_per_s = (0.2 + 0.3 * std::abs(wave(t, 17, 0.2))) * 1048576.0;
  eno1.tx_bytes_per_s = (0.1 + 0.2 * std::abs(wave(t, 23, 0.7))) * 1048576.0;
  NetIfSample& wlo1 = out.net.interfaces[1];
  wlo1.rx_bytes_per_s = (1.0 + 2.0 * std::abs(wave(t, 31, 0.3))) * 1048576.0;
  wlo1.tx_bytes_per_s = (0.3 + 0.4 * std::abs(wave(t, 41, 0.9))) * 1048576.0;
  out.net.total_rx_bytes_per_s = *eno1.rx_bytes_per_s + *wlo1.rx_bytes_per_s;
  out.net.total_tx_bytes_per_s = *eno1.tx_bytes_per_s + *wlo1.tx_bytes_per_s;
}

}  // namespace geth
