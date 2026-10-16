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
#include <optional>
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

  // Override the timestamp of the NEXT sample() only (consumed once). The
  // demo's --render-frame warm-up uses this as a virtual clock: the samples
  // run back-to-back in wall time but are stamped --interval apart, so 60 of
  // them fill the whole 60 s graph window instead of one millisecond of it.
  // Sources that read out.time (the demo) follow the virtual clock too.
  void set_sample_time(TimePoint t) { _fixed_time = t; }

  const SeriesStore& store() const { return _store; }
  const Snapshot& latest() const { return _snap; }

  // The sampling interval in ms, used to size the suspend-gap threshold: a
  // tick whose CLOCK_BOOTTIME is more than 5 x the interval past the last
  // one (the machine was asleep) records NaN for every RATE-based series
  // (CPU usage, counter-based CPU power, disk, network) instead of the
  // spike the sources would compute over a ~zero dt. Instantaneous readings
  // (temperatures, clocks, APU PPT, absolute byte counts, GPU usage/power)
  // are unaffected. Call once at startup; the UI updates it when the user
  // changes the interval.
  void set_interval_ms(long long ms) { _interval_ms = ms; }
  // Test hook: override the gap threshold (default: 5 x the interval).
  void set_gap_threshold_ms(long long ms) { _gap_threshold_ms = ms; }

 private:
  // Derive the series-id set from one sample (GPU bus ids, interface and
  // device names) and initialize the store. Runs once, after warm-up.
  void init_series();
  void record(const Snapshot& s);
  // Replace the RATE-based fields of a post-suspend tick with NaN (a gap in
  // the graph), keeping their nullopt-ness when the source reported nothing.
  void blank_gap_rates(Snapshot& s);
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
  // Timestamp for the next sample() (see set_sample_time); nullopt = Clock::now().
  std::optional<TimePoint> _fixed_time;
  // Suspend-gap detection (see set_interval_ms).
  long long _interval_ms = 1000;
  long long _gap_threshold_ms = -1;  // -1 = 5 x the interval
  std::optional<std::chrono::time_point<SuspendClock>> _last_suspend;
};

}  // namespace geth
