// Phase 5, Part C — suspend / clock-jump robustness: when the time between
// two samples exceeds the gap threshold (5x the interval, or the explicit
// set_gap_threshold_ms override), every RATE-based reading (deltas of
// counters) is blanked to NaN for that tick instead of reporting a huge
// spike. Absolute readings (memory, temperatures) are untouched, and the
// next tick after a normal interval reports finite values again.

#include <doctest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

#include <unistd.h>

#include "core/collector.hpp"
#include "core/ring_buffer.hpp"
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

// v1.5.3 — hotplug REMOVAL must never crash or repeat a stale value: an
// interface or disk that vanishes from the kernel's counters mid-run shows up
// as a gap (nullopt in the snapshot, NaN in the graph store — which is also
// what makes the CSV cell empty). The survivor is unaffected. The removal is
// simulated with the sources' test seams (set_test_netdev / set_test_diskstats),
// which stand in for the kernel updating /proc/net/dev and /proc/diskstats.
TEST_SUITE("v153_hotplug_removal") {

namespace {
// A throwaway <root>/class/net stand-in: <root>/<if>/device + <if>/type so the
// NetSource filter includes the named interfaces. remove_all'd on destruction.
struct FakeNetSysfs {
  std::filesystem::path root;
  FakeNetSysfs() {
    static int next = 0;
    root = std::filesystem::temp_directory_path() /
           ("geth_fakenet_" + std::to_string(::getpid()) + "_" +
            std::to_string(++next));
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
  }
  ~FakeNetSysfs() { std::filesystem::remove_all(root); }
  void add_iface(const std::string& name) {
    const auto d = root / name;
    std::filesystem::create_directories(d);
    std::ofstream(d / "device") << "0000:00:00.0";  // a backing PCI device
    std::ofstream(d / "type") << "1\n";              // ARPHRD_ETHER
  }
};
}  // namespace

TEST_CASE("removing an interface and a disk mid-run is a gap, never stale/crash") {
  FakeNetSysfs sysfs;
  sysfs.add_iface("eth0");
  sysfs.add_iface("eth1");

  // /proc/net/dev text: <if>: RX …(7 fields)… TX …; field 0 = rx bytes,
  // field 8 = tx bytes (parse_netdev / netdev_counters).
  auto netdev = [](unsigned long long e0rx, unsigned long long e0tx,
                   bool have_eth1, unsigned long long e1rx = 0,
                   unsigned long long e1tx = 0) {
    std::string s = "Inter-|   Receive   |  Transmit\n"
                    " face |bytes  packets|bytes  packets\n";
    s += "  eth0: " + std::to_string(e0rx) + " 10 0 0 0 0 0 0 " +
         std::to_string(e0tx) + " 10 0 0 0 0 0 0\n";
    if (have_eth1)
      s += "  eth1: " + std::to_string(e1rx) + " 20 0 0 0 0 0 0 " +
           std::to_string(e1tx) + " 20 0 0 0 0 0 0\n";
    return s;
  };
  // /proc/diskstats text: maj min NAME reads merges_r SECTORS_READ ms_r
  // writes merges_w SECTORS_WRITTEN …; field 2 = name, 5 = sectors read,
  // 9 = sectors written (parse_diskstats / diskstat_row).
  auto diskstats = [](unsigned long long sda_r, unsigned long long sda_w,
                      bool have_sdb, unsigned long long sdb_r = 0,
                      unsigned long long sdb_w = 0) {
    std::string s = "   8       0 sda 100 0 " + std::to_string(sda_r) +
                    " 100 50 0 " + std::to_string(sda_w) + " 0 0 0 0\n";
    if (have_sdb)
      s += "   8       1 sdb 200 0 " + std::to_string(sdb_r) +
           " 200 70 0 " + std::to_string(sdb_w) + " 0 0 0 0\n";
    return s;
  };

  auto net = std::make_unique<NetSource>(sysfs.root.string());
  NetSource* netp = net.get();
  auto disk = std::make_unique<DiskSource>();
  DiskSource* diskp = disk.get();
  netp->set_test_netdev(netdev(1000, 500, /*have_eth1=*/true, 2000, 1000));
  diskp->set_test_diskstats(diskstats(100, 50, /*have_sdb=*/true, 200, 70));

  Collector c;
  c.add(std::move(net));
  c.add(std::move(disk));
  c.set_interval_ms(10);        // short-dt guard = 5 ms; the 20 ms ticks clear it
  c.set_gap_threshold_ms(1000); // a 20 ms tick is never a suspend gap
  c.probe_all(/*warm_up=*/true);
  REQUIRE(c.any_available());

  // The probed set is fixed: eth0+eth1 and sda+sdb each got a slot.
  REQUIRE_EQ(c.latest().net.interfaces.size(), 2u);
  REQUIRE_EQ(c.latest().disk.devices.size(), 2u);

  const auto present = [](const std::optional<double>& v) {
    return v.has_value() && std::isfinite(*v);
  };
  const auto gap = [](const std::optional<double>& v) {
    return !v.has_value() || std::isnan(*v);
  };

  // Tick 1 (first live): rates are blanked (the baseline predates init).
  netp->set_test_netdev(netdev(100000, 50000, true, 200000, 100000));
  diskp->set_test_diskstats(diskstats(10000, 5000, true, 20000, 7000));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const Snapshot& t1 = c.sample();
  (void)t1;

  // Tick 2: both interfaces and both disks are present and finite.
  netp->set_test_netdev(netdev(200000, 100000, true, 400000, 200000));
  diskp->set_test_diskstats(diskstats(20000, 10000, true, 40000, 14000));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const Snapshot& t2 = c.sample();
  REQUIRE_EQ(t2.net.interfaces[0].name, "eth0");
  REQUIRE_EQ(t2.net.interfaces[1].name, "eth1");
  REQUIRE_EQ(t2.disk.devices[0].name, "sda");
  REQUIRE_EQ(t2.disk.devices[1].name, "sdb");
  CHECK(present(t2.net.interfaces[0].rx_bytes_per_s));
  CHECK(present(t2.net.interfaces[1].rx_bytes_per_s));
  CHECK(present(t2.disk.devices[0].read_bytes_per_s));
  CHECK(present(t2.disk.devices[1].read_bytes_per_s));
  const double eth1_rx_before = *t2.net.interfaces[1].rx_bytes_per_s;
  const double sdb_r_before = *t2.disk.devices[1].read_bytes_per_s;

  // The removal: eth1 and sdb vanish from the (fake) kernel's counters.
  netp->set_test_netdev(netdev(300000, 150000, /*have_eth1=*/false));
  diskp->set_test_diskstats(diskstats(30000, 15000, /*have_sdb=*/false));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  const Snapshot& t3 = c.sample();

  // eth0 and sda survive, still finite.
  CHECK(present(t3.net.interfaces[0].rx_bytes_per_s));
  CHECK(present(t3.disk.devices[0].read_bytes_per_s));
  // eth1 and sdb are a GAP — nullopt/NaN, never a repeat of the previous value.
  CHECK(gap(t3.net.interfaces[1].rx_bytes_per_s));
  CHECK(gap(t3.net.interfaces[1].tx_bytes_per_s));
  CHECK(gap(t3.disk.devices[1].read_bytes_per_s));
  CHECK(gap(t3.disk.devices[1].write_bytes_per_s));
  if (t3.net.interfaces[1].rx_bytes_per_s)
    CHECK(*t3.net.interfaces[1].rx_bytes_per_s != eth1_rx_before);
  if (t3.disk.devices[1].read_bytes_per_s)
    CHECK(*t3.disk.devices[1].read_bytes_per_s != sdb_r_before);

  // The graph store agrees: the removed series' newest sample is NaN (a gap),
  // the survivor's is finite.
  const RingBuffer<float>* eth1_rx = c.store().values("net.eth1.rx");
  const RingBuffer<float>* eth0_rx = c.store().values("net.eth0.rx");
  const RingBuffer<float>* sdb_r = c.store().values("disk.sdb.r");
  const RingBuffer<float>* sda_r = c.store().values("disk.sda.r");
  REQUIRE(eth1_rx);
  REQUIRE(eth0_rx);
  REQUIRE(sdb_r);
  REQUIRE(sda_r);
  CHECK(std::isnan(eth1_rx->at(eth1_rx->size() - 1)));
  CHECK(std::isnan(sdb_r->at(sdb_r->size() - 1)));
  CHECK(std::isfinite(eth0_rx->at(eth0_rx->size() - 1)));
  CHECK(std::isfinite(sda_r->at(sda_r->size() - 1)));
}

}  // TEST_SUITE
