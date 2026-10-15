// Owns all data sources, drives them each tick, and records per-series graph
// history in a SeriesStore (floats + one shared timestamp ring — no strings
// or vectors inside any ring, zero allocations per tick after init).
//
// The SAME Snapshot object is reused every tick: sources fill it in place
// into containers they sized on their first sample. sample() therefore
// returns a reference that stays valid until the next tick.
#pragma once

#include "core/series_store.hpp"
#include "core/snapshot.hpp"
#include "core/source.hpp"

#include <memory>
#include <string>
#include <vector>

namespace geth {

class Collector {
 public:
  // Register a source, in the order sources should run each tick. Call
  // before probe_all(). main.cpp wires the live sources this way; the tests
  // use it to inject fakes (e.g. a fake integrated GPU for the APU-PPT lift).
  void add(std::unique_ptr<Source> source) {
    _sources.push_back(std::move(source));
  }

  // Probes every source and records its status. With warm_up == true (all
  // live modes) it then takes one throw-away sample so counter-based metrics
  // — RAPL power, CPU usage, disk/net rates — have a reference value by the
  // time the caller shows the first real sample. The warm-up sample also
  // sizes the reused Snapshot's containers and derives the SeriesStore's
  // id set, so every later tick is allocation-free.
  void probe_all(bool warm_up);

  const std::vector<SourceStatus>& statuses() const { return _statuses; }
  bool any_available() const;

  // Takes one sample into the reused Snapshot, finalizes the APU-PPT lift,
  // and records it into the SeriesStore.
  const Snapshot& sample();

  const SeriesStore& store() const { return _store; }
  const Snapshot& latest() const { return _snap; }

 private:
  // Derive the series-id set from one sample (GPU bus ids, interface and
  // device names) and initialize the store. Runs once, after warm-up.
  void init_series();
  void record(const Snapshot& s);
  // Lift an active integrated AMD GPU's PPT into CPU package power when no
  // counter-based source won (see collector.cpp for why).
  void apply_apu_package_power(Snapshot& s);

  struct GpuSeriesIds {
    SeriesId usage, vram, gtt, core_clock, mem_clock, temp, power;
  };

  std::vector<std::unique_ptr<Source>> _sources;
  std::vector<SourceStatus> _statuses;
  Snapshot _snap;                      // reused every tick (filled in place)
  SeriesStore _store;
  std::vector<GpuSeriesIds> _gpu_series;  // aligned with _snap.gpus
  std::vector<SeriesId> _net_rx, _net_tx;     // aligned with _snap.net.interfaces
  std::vector<SeriesId> _disk_r, _disk_w;     // aligned with _snap.disk.devices
  bool _series_inited = false;
};

}  // namespace geth
