#include "core/collector.hpp"

#include "sources/gpu_amd.hpp"  // finalize_gpus()

namespace geth {

void Collector::probe_all(bool warm_up) {
  _statuses.clear();
  for (auto& s : _sources) _statuses.push_back(s->probe());
  if (warm_up && any_available()) {
    Snapshot warm;
    for (auto& s : _sources) s->sample(warm);
  }
}

bool Collector::any_available() const {
  for (const auto& s : _statuses)
    if (s.available) return true;
  return false;
}

Snapshot Collector::sample() {
  Snapshot s;
  for (auto& src : _sources) src->sample(s);
  // Assign stable gpu0/gpu1/... ids (PCI bus order). is_integrated and
  // integrated_method are decided by the GPU sources themselves.
  finalize_gpus(s.gpus);
  apply_apu_package_power(s);
  record(s);
  return s;
}

// AMD APU package power: the amdgpu PPT reading of an integrated GPU is the
// WHOLE-PACKAGE power (CPU + iGPU + SoC) — proven by 44-51 W during a
// CPU-only compile with the iGPU at 0%. So when no counter-based source
// (RAPL/amd_energy/zenpower) produced a value, an active integrated AMD
// GPU's PPT IS the CPU package power.
void Collector::apply_apu_package_power(Snapshot& s) {
  if (s.cpu.package_power_w) return;  // a counter source already won
  for (const auto& g : s.gpus) {
    if (g.vendor != GpuVendor::kAmd || !g.is_integrated) continue;
    if (g.state != GpuState::kActive || !g.ppt_w) continue;
    s.cpu.package_power_w = *g.ppt_w;
    s.cpu.package_power_source = CpuPackagePowerSource::kApuPpt;
    break;
  }
}

void Collector::record(const Snapshot& s) {
  auto put = [&](History<double>& h, const std::optional<double>& v) {
    if (v) h.push(s.time, *v);
  };
  put(_hist.cpu_usage, s.cpu.usage_pct);
  put(_hist.cpu_freq_avg, s.cpu.freq_avg_mhz);
  put(_hist.cpu_temp, s.cpu.temp_c);
  put(_hist.cpu_power, s.cpu.package_power_w);
  put(_hist.mem_used, s.mem.ram_used_bytes);
  put(_hist.swap_used, s.mem.swap_used_bytes);
  put(_hist.disk_read, s.disk.total_read_bytes_per_s);
  put(_hist.disk_write, s.disk.total_write_bytes_per_s);
  put(_hist.net_rx, s.net.total_rx_bytes_per_s);
  put(_hist.net_tx, s.net.total_tx_bytes_per_s);

  if (_hist.gpus.size() < s.gpus.size()) _hist.gpus.resize(s.gpus.size());
  for (std::size_t i = 0; i < s.gpus.size(); ++i) {
    auto& g = _hist.gpus[i];
    put(g.usage, s.gpus[i].usage_pct);
    put(g.vram_used, s.gpus[i].vram_used_bytes);
    put(g.core_clock, s.gpus[i].core_clock_mhz);
    put(g.mem_clock, s.gpus[i].mem_clock_mhz);
    put(g.power, s.gpus[i].power_w);
    put(g.temp, s.gpus[i].temp_c);
  }
}

}  // namespace geth
