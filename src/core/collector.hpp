// Owns all data sources, drives them each tick, and keeps the per-metric
// history buffers the later UI phases will graph.
#pragma once

#include "core/history.hpp"
#include "core/snapshot.hpp"
#include "core/source.hpp"

#include <memory>
#include <vector>

namespace geth {

class Collector {
 public:
  // 3600 samples at the default 1 s interval: one hour of graph data.
  static constexpr std::size_t kHistoryCapacity = 3600;

  void add(std::unique_ptr<Source> source) { _sources.push_back(std::move(source)); }
  bool has_sources() const { return !_sources.empty(); }

  // Probes every source and records its status. With warm_up == true (all
  // live modes) it then takes one throw-away sample so counter-based metrics
  // — RAPL power, CPU usage, disk/net rates — have a reference value by the
  // time the caller shows the first real sample.
  void probe_all(bool warm_up);

  const std::vector<SourceStatus>& statuses() const { return _statuses; }
  bool any_available() const;

  // Takes one sample, finalizes GPU ids, records it into history.
  Snapshot sample();

  struct GpuHistories {
    History<double> usage{kHistoryCapacity};
    History<double> vram_used{kHistoryCapacity};
    History<double> core_clock{kHistoryCapacity};
    History<double> mem_clock{kHistoryCapacity};
    History<double> power{kHistoryCapacity};
    History<double> temp{kHistoryCapacity};
  };

  struct Histories {
    History<double> cpu_usage{kHistoryCapacity};
    History<double> cpu_freq_avg{kHistoryCapacity};
    History<double> cpu_temp{kHistoryCapacity};
    History<double> cpu_power{kHistoryCapacity};
    History<double> mem_used{kHistoryCapacity};
    History<double> swap_used{kHistoryCapacity};
    History<double> disk_read{kHistoryCapacity};
    History<double> disk_write{kHistoryCapacity};
    History<double> net_rx{kHistoryCapacity};
    History<double> net_tx{kHistoryCapacity};
    std::vector<GpuHistories> gpus;  // aligned with Snapshot::gpus
  };

  const Histories& histories() const { return _hist; }

 private:
  void record(const Snapshot& s);
  // Lift an active integrated AMD GPU's PPT into CPU package power when no
  // counter-based source won (see collector.cpp for why).
  void apply_apu_package_power(Snapshot& s);

  std::vector<std::unique_ptr<Source>> _sources;
  std::vector<SourceStatus> _statuses;
  Histories _hist;
};

}  // namespace geth
