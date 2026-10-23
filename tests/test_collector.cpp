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
  // The steady ticks below are ~10 ms apart; keep the short-dt guard (half an
  // interval) well under that so it does not blank them.
  c.set_interval_ms(10);

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
  c.set_interval_ms(10);  // keep the short-dt guard (5 ms) under the ticks

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

// v1.5.2 — finding 5 (stale values in the reused snapshot), finding 6
// (CPU usage freezes after a core-count change), finding 7 (the first live
// tick's rate sample includes GETH's own startup work). Each test is written
// to FAIL on the pre-fix code and PASS after the fix.
TEST_SUITE("v152_stale_first_tick") {

namespace {
// Reports cpu.usage on exactly one tick (call 3) and then stops, simulating
// a read failure. With the per-tick snapshot reset, the following tick's
// value must be ABSENT — never a repeat of the reported value.
struct VanishingSource : Source {
  int calls = 0;
  const char* name() const override { return "vanishing (test)"; }
  SourceStatus probe() override {
    SourceStatus st;
    st.available = true;
    st.description = name();
    return st;
  }
  void sample(Snapshot& out) override {
    ++calls;  // 1 = warm-up, 2 = first live (discarded), 3 = value, 4+ = fail
    if (calls == 3) out.cpu.usage_pct = 42.0;
    // every other call leaves usage absent (a read failure)
  }
};
}  // namespace

TEST_CASE("finding 5: a read failure records an absence, never the previous value") {
  Collector c;
  c.add(std::make_unique<VanishingSource>());
  c.probe_all(/*warm_up=*/true);  // call 1 (warm-up)
  TimePoint t = c.latest().time;
  t += std::chrono::seconds(1);  c.set_sample_time(t);
  c.sample();                     // call 2: first live tick, discarded (finding 7)
  t += std::chrono::seconds(1);  c.set_sample_time(t);
  const Snapshot& got = c.sample();  // call 3: usage = 42
  REQUIRE(got.cpu.usage_pct.has_value());
  CHECK_EQ(*got.cpu.usage_pct, 42.0);
  t += std::chrono::seconds(1);  c.set_sample_time(t);
  const Snapshot& lost = c.sample();  // call 4: read failure (no usage)
  CHECK(!lost.cpu.usage_pct.has_value());  // absent, not 42 repeated
}

TEST_CASE("finding 6: a core-count change re-baselines, never freezes usage") {
  auto cpu = std::make_unique<CpuSource>();
  CpuSource* cp = cpu.get();
  Collector c;
  c.add(std::move(cpu));
  const auto absent_or_nan = [](const std::optional<double>& v) {
    return !v.has_value() || std::isnan(*v);
  };
  // A /proc/stat text: one aggregate line (busy/idle jiffies) + `cores` cores.
  auto stat = [](int cores, unsigned long long busy, unsigned long long idle) {
    std::string s = "cpu  " + std::to_string(busy) + " 0 0 " + std::to_string(idle) +
                    " 0 0 0 0\n";
    for (int i = 0; i < cores; ++i)
      s += "cpu" + std::to_string(i) + " 0 0 0 0 0 0 0 0\n";
    return s;
  };
  cp->set_test_stat(stat(2, 100, 900));  // warm-up baseline: 2 cores
  c.probe_all(/*warm_up=*/true);
  TimePoint t = c.latest().time;
  // Discard the first live tick (its baseline predates init) to get past it.
  cp->set_test_stat(stat(2, 150, 950));
  t += std::chrono::seconds(1);  c.set_sample_time(t);
  (void)c.sample();
  // 2 cores: a clean 50 % delta.
  cp->set_test_stat(stat(2, 200, 1000));
  t += std::chrono::seconds(1);  c.set_sample_time(t);
  const Snapshot& s1 = c.sample();
  REQUIRE(s1.cpu.usage_pct.has_value());
  CHECK(std::abs(*s1.cpu.usage_pct - 50.0) < 1e-9);
  // A core appears (2 -> 3): re-baseline to NaN, not a repeat of 50.
  cp->set_test_stat(stat(3, 300, 1100));
  t += std::chrono::seconds(1);  c.set_sample_time(t);
  const Snapshot& s2 = c.sample();
  CHECK(absent_or_nan(s2.cpu.usage_pct));
  // Same 3 cores: a clean 33.33 % delta, not the frozen 50.
  cp->set_test_stat(stat(3, 400, 1300));
  t += std::chrono::seconds(1);  c.set_sample_time(t);
  const Snapshot& s3 = c.sample();
  REQUIRE(s3.cpu.usage_pct.has_value());
  CHECK(std::abs(*s3.cpu.usage_pct - 33.3333333333) < 1e-3);
}

TEST_CASE("finding 7: the first live tick's startup-work rate sample is NaN") {
  Collector c;
  c.add(std::make_unique<CpuSource>());
  c.probe_all(/*warm_up=*/true);
  REQUIRE(c.any_available());
  const auto absent_or_nan = [](const std::optional<double>& v) {
    return !v.has_value() || std::isnan(*v);
  };
  // > half an interval so the short-dt guard doesn't also fire.
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  const Snapshot& first = c.sample();  // baseline predates init -> discarded
  CHECK(absent_or_nan(first.cpu.usage_pct));
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  const Snapshot& second = c.sample();  // a clean delta: finite
  REQUIRE(second.cpu.usage_pct.has_value());
  CHECK(std::isfinite(*second.cpu.usage_pct));
}

}  // TEST_SUITE
