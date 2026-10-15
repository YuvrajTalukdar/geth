#include "core/collector.hpp"

#include "sources/gpu_amd.hpp"  // finalize_gpus()

namespace geth {

void Collector::probe_all(bool warm_up) {
  _statuses.clear();
  for (auto& s : _sources) _statuses.push_back(s->probe());
  if (!warm_up || !any_available()) return;

  // Throw-away sample: gives counter sources a baseline and sizes the
  // reused Snapshot's containers (per-core, per-GPU, per-interface,
  // per-device) exactly once. It does NOT record into the store — the
  // first real tick is tick 1 of the history.
  _snap.time = Clock::now();
  for (auto& s : _sources) s->sample(_snap);
  // GPU identity (id, is_integrated, integrated_method) is a property of the
  // hardware, not of a tick: decide it once here, never per tick.
  finalize_gpus(_snap.gpus);
  // The probe snapshot must be as consistent as a real tick: lift the APU PPT
  // (or keep the RAPL/counter value if one won) so a caller that builds the UI
  // page from latest() — hardware_from() — sees the same cpu.package_power_* a
  // live frame would. Without this, the Power panel's CPU line is missing on a
  // machine where RAPL is unreadable and only the integrated GPU's PPT exists.
  apply_apu_package_power(_snap);
  init_series();
}

bool Collector::any_available() const {
  for (const auto& s : _statuses)
    if (s.available) return true;
  return false;
}

const Snapshot& Collector::sample() {
  // A fixed timestamp (set_sample_time, demo virtual clock) is consumed once;
  // otherwise sample at the real steady clock.
  const TimePoint now = _fixed_time ? *_fixed_time : Clock::now();
  _fixed_time.reset();
  _snap.time = now;
  for (auto& src : _sources) src->sample(_snap);
  apply_apu_package_power(_snap);
  _store.begin_tick(_snap.time);
  record(_snap);
  return _snap;
}

void Collector::init_series() {
  std::vector<SeriesId> ids;
  auto add = [&](SeriesId id) { ids.push_back(std::move(id)); };

  add("cpu.usage");
  add("cpu.temp");
  add("cpu.power");
  add("mem.ram_used");
  add("mem.swap_used");
  add("net.total.rx");
  add("net.total.tx");
  add("disk.total.r");
  add("disk.total.w");

  _gpu_series.resize(_snap.gpus.size());
  for (std::size_t i = 0; i < _snap.gpus.size(); ++i) {
    const auto& g = _snap.gpus[i];
    const std::string base = "gpu." + g.pci_bus_id + ".";
    auto& ids = _gpu_series[i];
    ids.usage = base + "usage";
    ids.vram = base + "vram";
    ids.gtt = base + "gtt";
    ids.core_clock = base + "core_clock";
    ids.mem_clock = base + "mem_clock";
    ids.temp = base + "temp";
    ids.power = base + "power";
    add(ids.usage);
    add(ids.vram);
    add(ids.gtt);
    add(ids.core_clock);
    add(ids.mem_clock);
    add(ids.temp);
    add(ids.power);
  }

  _net_rx.resize(_snap.net.interfaces.size());
  _net_tx.resize(_snap.net.interfaces.size());
  for (std::size_t i = 0; i < _snap.net.interfaces.size(); ++i) {
    _net_rx[i] = "net." + _snap.net.interfaces[i].name + ".rx";
    _net_tx[i] = "net." + _snap.net.interfaces[i].name + ".tx";
    add(_net_rx[i]);
    add(_net_tx[i]);
  }

  _disk_r.resize(_snap.disk.devices.size());
  _disk_w.resize(_snap.disk.devices.size());
  for (std::size_t i = 0; i < _snap.disk.devices.size(); ++i) {
    _disk_r[i] = "disk." + _snap.disk.devices[i].name + ".r";
    _disk_w[i] = "disk." + _snap.disk.devices[i].name + ".w";
    add(_disk_r[i]);
    add(_disk_w[i]);
  }

  _store.init(ids);
  _series_inited = true;
}

void Collector::record(const Snapshot& s) {
  if (!_series_inited) return;
  _store.put("cpu.usage", s.cpu.usage_pct);
  _store.put("cpu.temp", s.cpu.temp_c);
  _store.put("cpu.power", s.cpu.package_power_w);
  _store.put("mem.ram_used", s.mem.ram_used_bytes);
  _store.put("mem.swap_used", s.mem.swap_used_bytes);
  _store.put("net.total.rx", s.net.total_rx_bytes_per_s);
  _store.put("net.total.tx", s.net.total_tx_bytes_per_s);
  _store.put("disk.total.r", s.disk.total_read_bytes_per_s);
  _store.put("disk.total.w", s.disk.total_write_bytes_per_s);

  for (std::size_t i = 0; i < s.gpus.size() && i < _gpu_series.size(); ++i) {
    const auto& ids = _gpu_series[i];
    const auto& g = s.gpus[i];
    _store.put(ids.usage, g.usage_pct);
    _store.put(ids.vram, g.vram_used_bytes);
    _store.put(ids.gtt, g.gtt_used_bytes);
    _store.put(ids.core_clock, g.core_clock_mhz);
    _store.put(ids.mem_clock, g.mem_clock_mhz);
    _store.put(ids.temp, g.temp_c);
    _store.put(ids.power, g.power_w);
  }
  for (std::size_t i = 0; i < s.net.interfaces.size() && i < _net_rx.size(); ++i) {
    _store.put(_net_rx[i], s.net.interfaces[i].rx_bytes_per_s);
    _store.put(_net_tx[i], s.net.interfaces[i].tx_bytes_per_s);
  }
  for (std::size_t i = 0; i < s.disk.devices.size() && i < _disk_r.size(); ++i) {
    _store.put(_disk_r[i], s.disk.devices[i].read_bytes_per_s);
    _store.put(_disk_w[i], s.disk.devices[i].write_bytes_per_s);
  }
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

}  // namespace geth
