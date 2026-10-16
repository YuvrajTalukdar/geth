// Phase 5, Part C — suspend / clock-jump robustness: when the time between
// two samples exceeds the gap threshold (5x the interval, or the explicit
// set_gap_threshold_ms override), every RATE-based reading (deltas of
// counters) is blanked to NaN for that tick instead of reporting a huge
// spike. Absolute readings (memory, temperatures) are untouched, and the
// next tick after a normal interval reports finite values again.

#include <doctest.h>

#include <cmath>
#include <memory>
#include <thread>

#include "core/collector.hpp"
#include "sources/cpu.hpp"
#include "sources/disk.hpp"
#include "sources/memory.hpp"
#include "sources/net.hpp"

using namespace geth;

TEST_SUITE("collector_gap") {

TEST_CASE("a long gap between samples blanks all rates to NaN for that tick") {
  Collector c;
  c.add(std::make_unique<CpuSource>());
  c.add(std::make_unique<MemorySource>());
  c.add(std::make_unique<DiskSource>());
  c.add(std::make_unique<NetSource>());
  c.probe_all(/*warm_up=*/true);
  REQUIRE(c.any_available());
  // A gap is any inter-sample delta above this (well below the 400 ms jump).
  c.set_gap_threshold_ms(100);

  const auto finite_or_absent = [](const std::optional<double>& v) {
    return !v.has_value() || std::isfinite(*v);
  };
  // After a gap: a rate is either NaN or absent — never a finite spike.
  const auto blanked = [](const std::optional<double>& v) {
    return !v.has_value() || std::isnan(*v);
  };

  // --- baseline: two samples a short interval apart (no gap) -------------
  const Snapshot& a = c.sample();  // baseline only (CPU usage needs a delta)
  (void)a;
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const Snapshot& b = c.sample();
  REQUIRE(b.cpu.usage_pct.has_value());
  CHECK(std::isfinite(*b.cpu.usage_pct));
  CHECK(finite_or_absent(b.disk.total_read_bytes_per_s));
  CHECK(finite_or_absent(b.net.total_rx_bytes_per_s));
  CHECK(b.mem.ram_used_bytes.has_value());

  // --- the jump: 400 ms >> the 100 ms threshold (simulates sleep) --------
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  const Snapshot& g = c.sample();

  // Rate-based: NaN (or absent), never a finite spike.
  REQUIRE(g.cpu.usage_pct.has_value());  // it was present in the baseline
  CHECK(std::isnan(*g.cpu.usage_pct));
  for (const auto& core : g.cpu.per_core_pct) CHECK(blanked(core));
  CHECK(blanked(g.disk.total_read_bytes_per_s));
  CHECK(blanked(g.disk.total_write_bytes_per_s));
  CHECK(blanked(g.net.total_rx_bytes_per_s));
  CHECK(blanked(g.net.total_tx_bytes_per_s));
  for (const auto& d : g.disk.devices) {
    CHECK(blanked(d.read_bytes_per_s));
    CHECK(blanked(d.write_bytes_per_s));
  }
  for (const auto& n : g.net.interfaces) {
    CHECK(blanked(n.rx_bytes_per_s));
    CHECK(blanked(n.tx_bytes_per_s));
  }

  // Absolute: untouched by the gap.
  CHECK(b.mem.ram_used_bytes.has_value());
  CHECK(g.mem.ram_used_bytes.has_value());
  if (g.mem.ram_used_bytes) CHECK(!std::isnan(*g.mem.ram_used_bytes));
  if (g.cpu.temp_c) CHECK(std::isfinite(*g.cpu.temp_c));

  // --- recovery: a normal interval later, rates are finite again ---------
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const Snapshot& r = c.sample();
  REQUIRE(r.cpu.usage_pct.has_value());
  CHECK(std::isfinite(*r.cpu.usage_pct));
}

TEST_CASE("short intervals never register a gap (threshold is relative)") {
  Collector c;
  c.add(std::make_unique<CpuSource>());
  c.probe_all(/*warm_up=*/true);
  c.set_gap_threshold_ms(1000);  // 1 s: ten 10 ms ticks are far below it

  const Snapshot& a = c.sample();
  (void)a;
  for (int i = 0; i < 10; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const Snapshot& s = c.sample();
    CHECK(s.cpu.usage_pct.has_value());
    if (s.cpu.usage_pct) CHECK(std::isfinite(*s.cpu.usage_pct));
  }
}

}  // TEST_SUITE
