// Unit tests for the pure parsing/formatting helpers, the SeriesStore rings,
// the PCI id database and the GPU id/integration finalizer.
// Fixtures live in tests/fixtures (path passed via GETH_TEST_FIXTURES).

#include <doctest.h>

#include <unistd.h>

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <thread>

#include "util/alloc_count.hpp"
#include "core/collector.hpp"
#include "core/ring_buffer.hpp"
#include "core/series_store.hpp"
#include "core/snapshot.hpp"
#include "sources/cpu.hpp"
#include "sources/cpu_power.hpp"
#include "sources/demo.hpp"
#include "sources/disk.hpp"
#include "sources/gpu_amd.hpp"
#include "sources/gpu_nvidia.hpp"
#include "sources/gpu_registry.hpp"
#include "sources/pci_discovery.hpp"
#include "sources/memory.hpp"
#include "sources/net.hpp"
#include "sources/temps.hpp"
#include "fake_pci_tree.hpp"
#include "ui/panel_spec.hpp"
#include "util/format.hpp"
#include "util/pci_ids.hpp"

using namespace geth;

namespace {

std::string read_fixture(const char* name) {
  std::ifstream f(std::string(GETH_TEST_FIXTURES) + "/" + name);
  DOCTEST_REQUIRE(f.good());
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// A minimal /sys/class/net stand-in for the interface-filter tests: each
// entry is a directory with the sysfs attribute files the filter reads.
// "device" is a plain file here (the real one is a symlink);
// std::filesystem::exists() does not care.
struct FakeNetTree {
  explicit FakeNetTree(const std::string& dir) : root(dir) {
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
  }
  ~FakeNetTree() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  FakeNetTree(const FakeNetTree&) = delete;
  FakeNetTree& operator=(const FakeNetTree&) = delete;

  void add(const std::string& name, const std::string& type = "", bool has_device = false,
           bool has_bridge = false) {
    std::filesystem::create_directories(root + "/" + name);
    if (!type.empty()) std::ofstream(root + "/" + name + "/type") << type << "\n";
    if (has_device) std::ofstream(root + "/" + name + "/device") << "fake device\n";
    if (has_bridge) std::ofstream(root + "/" + name + "/bridge") << "1\n";
  }

  std::string root;
};

}  // namespace

// ---------------------------------------------------------------------------
// /proc/stat
// ---------------------------------------------------------------------------

TEST_SUITE("cpu") {
  TEST_CASE("parse_proc_stat picks cpu lines only") {
    const auto rows = parse_proc_stat(read_fixture("proc_stat_before.txt"));
    DOCTEST_REQUIRE(rows.size() == 3);  // aggregate + cpu0 + cpu1
    // First 8 fields only; guest/guest_nice (300/40) must not be counted.
    CHECK(rows[0].total() == 1000ULL + 200 + 300 + 4000 + 500 + 100 + 50 + 20);
    CHECK(rows[0].idle_all() == 4000 + 500);
  }

  TEST_CASE("usage_pct_between matches hand-computed value") {
    const auto a = parse_proc_stat(read_fixture("proc_stat_before.txt"));
    const auto b = parse_proc_stat(read_fixture("proc_stat_after.txt"));
    DOCTEST_REQUIRE(a.size() == 3);
    DOCTEST_REQUIRE(b.size() == 3);

    // aggregate: dtotal = 6627-6170 = 457, dbusy = 197 -> 197/457*100
    const auto pct = usage_pct_between(a[0], b[0]);
    DOCTEST_REQUIRE(pct.has_value());
    CHECK(*pct == doctest::Approx(100.0 * 197.0 / 457.0).epsilon(1e-9));

    // cpu1: dtotal = 698-617 = 81, dbusy = 200-167 = 33
    const auto p1 = usage_pct_between(a[2], b[2]);
    DOCTEST_REQUIRE(p1.has_value());
    CHECK(*p1 == doctest::Approx(100.0 * 33.0 / 81.0).epsilon(1e-9));
  }

  TEST_CASE("usage_pct_between rejects non-advancing counters") {
    CpuTimes x;
    CHECK(usage_pct_between(x, x).has_value() == false);  // dtotal == 0
  }

  TEST_CASE("parse_cpuinfo_mhz collects cpu MHz lines") {
    const auto mhz = parse_cpuinfo_mhz(read_fixture("cpuinfo.txt"));
    DOCTEST_REQUIRE(mhz.size() == 2);
    CHECK(mhz[0] == doctest::Approx(4191.154));
    CHECK(mhz[1] == doctest::Approx(3900.0));
  }

  TEST_CASE("parse_cpuinfo_model_name takes the first model name line") {
    const auto name = parse_cpuinfo_model_name(read_fixture("cpuinfo.txt"));
    DOCTEST_REQUIRE(name.has_value());
    CHECK(*name == "AMD Ryzen 5 5600H with Radeon Graphics");
    CHECK(parse_cpuinfo_model_name("processor\t: 0\nvendor_id\t: AuthenticAMD\n")
              .has_value() == false);
    CHECK(parse_cpuinfo_model_name("").has_value() == false);
  }

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// /proc/meminfo
// ---------------------------------------------------------------------------

TEST_SUITE("memory") {
  TEST_CASE("parse_meminfo extracts the four keys") {
    const auto mi = parse_meminfo(read_fixture("meminfo.txt"));
    DOCTEST_REQUIRE(mi.ok);
    CHECK(mi.mem_total_kb == 32768000ULL);
    CHECK(mi.mem_available_kb == 20480000ULL);
    CHECK(mi.swap_total_kb == 8388608ULL);
    CHECK(mi.swap_free_kb == 8000000ULL);
  }

  TEST_CASE("parse_meminfo rejects garbage") {
    const auto mi = parse_meminfo("Bogus: 1 kB\n");
    CHECK(mi.ok == false);
  }

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// CPU power counter math
// ---------------------------------------------------------------------------

TEST_SUITE("cpu_power") {
  TEST_CASE("counter_delta without wrap") {
    CHECK(counter_delta(1000, 400, 100000).value() == 600ULL);
  }

  TEST_CASE("counter_delta with single wrap") {
    // scale 100: prev 95, now 5 -> wrapped once, delta 10
    CHECK(counter_delta(5, 95, 100).value() == 10ULL);
  }

  TEST_CASE("counter_delta rejects a jump beyond one full scale") {
    // 95 -> 250 is 155 with scale 100: that is "reset", not "1.5 wraps".
    CHECK(counter_delta(250, 95, 100).has_value() == false);
    // No scale published (max_range 0) and now < prev: the wrap distance is
    // unknowable, refuse rather than guess.
    CHECK(counter_delta(5, 95, 0).has_value() == false);
  }

  TEST_CASE("counter_delta without published scale assumes no wrap") {
    CHECK(counter_delta(1000, 400, 0).value() == 600ULL);
  }

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// Temperature parsing
// ---------------------------------------------------------------------------

TEST_SUITE("temps") {
  TEST_CASE("parse_millidegrees valid values") {
    CHECK(parse_millidegrees("59000").value() == 59.0);
    CHECK(parse_millidegrees("  42500 ").value() == 42.5);
  }

  TEST_CASE("parse_millidegrees rejects no-data and garbage") {
    CHECK(parse_millidegrees("-127000").has_value() == false);  // hwmon "no data"
    CHECK(parse_millidegrees("").has_value() == false);
    CHECK(parse_millidegrees("abc").has_value() == false);
    CHECK(parse_millidegrees("99999999999").has_value() == false);  // not physical
  }

  TEST_CASE("choose_temp_index honours the per-driver priority") {
    std::map<std::size_t, std::string> k10{
        {1, "Tctl"},
        {2, "Tdie"},
    };
    CHECK(choose_temp_index("k10temp", k10).value() == 2);  // Tdie wins over Tctl

    std::map<std::size_t, std::string> k10b{{1, "Tctl"}};
    CHECK(choose_temp_index("k10temp", k10b).value() == 1);  // Tctl is fine

    std::map<std::size_t, std::string> core{
        {1, "Core 0"},
        {2, "Package id 0"},
    };
    CHECK(choose_temp_index("coretemp", core).value() == 2);

    std::map<std::size_t, std::string> acpi{{1, ""}};
    CHECK(choose_temp_index("acpitz", acpi).value() == 1);

    CHECK(choose_temp_index("bogus", k10).has_value() == false);
  }

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// Disk + network
// ---------------------------------------------------------------------------

TEST_SUITE("disk") {
  TEST_CASE("parse_diskstats takes sector fields at the right indices") {
    const auto rows = parse_diskstats(read_fixture("diskstats.txt"));
    const auto find = [&](const char* n) -> const DiskRow* {
      for (const auto& r : rows)
        if (r.name == n) return &r;
      return nullptr;
    };
    DOCTEST_REQUIRE(find("nvme0n1") != nullptr);
    CHECK(find("nvme0n1")->sectors_read == 80000ULL);
    CHECK(find("nvme0n1")->sectors_written == 160000ULL);
    DOCTEST_REQUIRE(find("sda") != nullptr);
    CHECK(find("sda")->sectors_read == 40000ULL);
    CHECK(find("sda")->sectors_written == 80000ULL);
  }

  TEST_CASE("is_partition detects children of listed devices") {
    const std::vector<std::string> all = {"nvme0n1", "nvme0n1p1", "sda", "sda1", "loop0"};
    CHECK(is_partition("nvme0n1p1", all));
    CHECK(is_partition("sda1", all));
    CHECK(!is_partition("nvme0n1", all));
    CHECK(!is_partition("sda", all));
  }

  TEST_CASE("skip_disk_device filters virtual and virtual-like devices") {
    CHECK(skip_disk_device("loop3"));
    CHECK(skip_disk_device("ram0"));
    CHECK(skip_disk_device("dm-2"));
    CHECK(skip_disk_device("sr0"));
    CHECK(skip_disk_device("md127"));
    CHECK(!skip_disk_device("nvme0n1"));
    CHECK(!skip_disk_device("sda"));
  }

}  // TEST_SUITE

TEST_SUITE("net") {
  TEST_CASE("parse_netdev skips headers and finds rx/tx bytes") {
    const auto rows = parse_netdev(read_fixture("netdev.txt"));
    const auto find = [&](const char* n) -> const NetRow* {
      for (const auto& r : rows)
        if (r.name == n) return &r;
      return nullptr;
    };
    DOCTEST_REQUIRE(rows.size() == 4);
    DOCTEST_REQUIRE(find("eno1") != nullptr);
    CHECK(find("eno1")->rx_bytes == 2000000ULL);
    CHECK(find("eno1")->tx_bytes == 3000000ULL);
    CHECK(find("lo") != nullptr);  // parsing keeps lo; the source filters it
  }

  TEST_CASE("net_iface_included keeps only physical Ethernet/wireless") {
    const std::string dir = (std::filesystem::temp_directory_path() /
                             ("geth_test_net_" + std::to_string(::getpid())))
                                .string();
    FakeNetTree net(dir);
    net.add("eno1", "1", /*has_device=*/true);
    net.add("wlo1", "801", /*has_device=*/true);
    net.add("lo", "772");
    net.add("docker0", "1");  // no device link: virtual
    net.add("br0", "1", /*has_device=*/true, /*has_bridge=*/true);
    net.add("vmnet", "772", /*has_device=*/true);  // device, but not Ethernet
    net.add("veth0abc", "1");
    net.add("tun0", "1");
    net.add("tap0", "1");
    net.add("wg0", "1");
    net.add("br-eth0", "1");
    net.add("virbr0", "1");

    NetFilterResult res = net_iface_included(net.root, "eno1");
    CHECK(res.included);
    CHECK(res.reason.empty());

    res = net_iface_included(net.root, "wlo1");
    CHECK(res.included);  // 801 = IEEE 802.11

    res = net_iface_included(net.root, "lo");
    CHECK(!res.included);
    CHECK(res.reason == "loopback");

    for (const char* v : {"docker0", "veth0abc", "tun0", "tap0", "wg0", "br-eth0",
                          "virbr0"}) {
      res = net_iface_included(net.root, v);
      CHECK(!res.included);
      CHECK(res.reason == "virtual interface");
    }

    res = net_iface_included(net.root, "br0");
    CHECK(!res.included);
    CHECK(res.reason == "bridge");

    res = net_iface_included(net.root, "vmnet");
    CHECK(!res.included);
    CHECK(res.reason == "type 772 is not Ethernet/wireless");

    res = net_iface_included(net.root, "nonexistent0");
    CHECK(!res.included);
    CHECK(res.reason == "no backing device");
  }

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// Series store (per-series rings: floats + one shared timestamp ring)
// ---------------------------------------------------------------------------

TEST_SUITE("series_store") {
  TEST_CASE("ring buffer overwrites oldest and keeps order") {
    RingBuffer<double> h(4);
    for (int i = 0; i < 10; ++i) h.push(static_cast<double>(i));
    CHECK(h.size() == 4);
    CHECK(h.at(0) == 6.0);  // 0..5 were evicted
    CHECK(h.at(3) == 9.0);
  }

  TEST_CASE("ring buffer clear/empty") {
    RingBuffer<double> h(3);
    CHECK(h.empty());
    h.push(1.0);
    CHECK(!h.empty());
    h.clear();
    CHECK(h.empty());
    CHECK(h.size() == 0);
  }

  TEST_CASE("missing samples are stored as NaN, never as 0") {
    SeriesStore s;
    s.init({"x"});
    s.begin_tick(Clock::now());
    s.put("x", 1.0);
    s.begin_tick(Clock::now());
    s.put("x", std::nullopt);
    const auto* v = s.values("x");
    REQUIRE(v != nullptr);
    CHECK(v->size() == 2);
    CHECK(v->at(0) == 1.0f);
    CHECK(std::isnan(v->at(1)));
  }

  TEST_CASE("shared timestamp ring advances once per tick") {
    SeriesStore s;
    s.init({"x", "y"});
    s.begin_tick(Clock::now());
    s.begin_tick(Clock::now());
    CHECK(s.times()->size() == 2);
  }

  TEST_CASE("unknown ids are dropped without growing the store") {
    SeriesStore s;
    s.init({"x"});
    s.begin_tick(Clock::now());
    s.put("never-seen", 1.0);  // hardware that appeared after init
    CHECK(s.series_count() == 1);
    CHECK(s.has("never-seen") == false);
    CHECK(s.values("never-seen") == nullptr);
    CHECK(s.values("x")->size() == 1);  // the known series still took its tick
  }

  TEST_CASE("sampling path: zero heap allocations per tick after warm-up") {
    // The whole point of the reused-Snapshot + SeriesStore design: after the
    // first tick the sampling path (source fill + APU lift + store puts)
    // performs no heap allocations at all.
    geth::set_alloc_counting(true);
    Collector c;
    c.add(std::make_unique<DemoSource>());
    c.probe_all(/*warm_up=*/true);  // one-time allocations (sizing, series ids)
    for (int i = 0; i < 5; ++i) c.sample();  // a few warm ticks
    const long long base = geth::alloc_count();
    for (int i = 0; i < 500; ++i) c.sample();
    CHECK(geth::alloc_count() == base);
    geth::set_alloc_counting(false);
  }

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// PCI ids
// ---------------------------------------------------------------------------

TEST_SUITE("pci_ids") {
  // The database is streamed once and only the requested (vendor, device)
  // pairs are retained — that is the whole point (no ~150k-entry map in RSS).
  TEST_CASE("load_subset keeps only the requested pairs, under the right vendor") {
    PciIds ids(std::string(GETH_TEST_FIXTURES) + "/pci_ids.txt");
    CHECK(ids.loaded() == false);  // nothing loaded before load_subset
    ids.load_subset({{0x1002, 0x1638}, {0x10de, 0x25a2}, {0x10ec, 0x1234}});
    DOCTEST_REQUIRE(ids.loaded());
    CHECK(ids.size() == 3);  // exactly the three requested, nothing else
    CHECK(ids.lookup(0x1002, 0x1638).value() ==
          "Cezanne [Radeon Vega Series / Radeon Vega Mobile Series]");
    CHECK(ids.lookup(0x10de, 0x25a2).value() == "GA107M [GeForce RTX 3050 Mobile]");
    CHECK(ids.lookup(0x10ec, 0x1234).value() == "I219-LM");
    // Same device id under a different vendor must not leak across.
    CHECK(ids.lookup(0x1234, 0x1638).has_value() == false);
    // A device present in the file but NOT requested is not retained.
    CHECK(ids.lookup(0x1002, 0x1681).has_value() == false);
  }

  TEST_CASE("load_subset is a no-op when called twice") {
    PciIds ids(std::string(GETH_TEST_FIXTURES) + "/pci_ids.txt");
    ids.load_subset({{0x1002, 0x1638}});
    const auto first = ids.size();
    ids.load_subset({{0x10de, 0x25a2}});  // must not grow the retained set
    CHECK(ids.size() == first);
  }

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// GPU finalization
// ---------------------------------------------------------------------------

TEST_SUITE("gpus") {
  TEST_CASE("finalize_gpus sorts by PCI bus and assigns ids") {
    std::vector<GpuSample> gpus(2);
    gpus[0].vendor = GpuVendor::kAmd;
    gpus[0].pci_bus_id = "0000:07:00.0";
    gpus[0].is_integrated = true;  // decided by the source; must survive
    gpus[0].integrated_method = "gpu_metrics format 2 (APU)";
    gpus[1].vendor = GpuVendor::kNvidia;
    gpus[1].pci_bus_id = "0000:01:00.0";
    finalize_gpus(gpus);
    CHECK(gpus[0].id == "gpu0");
    CHECK(gpus[1].id == "gpu1");
    CHECK(gpus[0].vendor == GpuVendor::kNvidia);  // "01:00.0" sorts before "07:00.0"
    CHECK(gpus[1].vendor == GpuVendor::kAmd);
    CHECK(gpus[1].is_integrated);        // finalize does not touch the flags
    CHECK(gpus[1].integrated_method == "gpu_metrics format 2 (APU)");
  }

  TEST_CASE("finalize_gpus: single GPU keeps its id and flags") {
    std::vector<GpuSample> gpus(1);
    gpus[0].vendor = GpuVendor::kAmd;
    gpus[0].pci_bus_id = "0000:02:00.0";
    gpus[0].is_integrated = true;
    finalize_gpus(gpus);
    CHECK(gpus[0].is_integrated);
    CHECK(gpus[0].id == "gpu0");
  }

  TEST_CASE("demo source fills the whole data model") {
    DemoSource demo;
    const auto st = demo.probe();
    DOCTEST_REQUIRE(st.available);
    // Sources append into the snapshot; the Collector makes a fresh one each
    // tick, so test each sample on its own snapshot.
    Snapshot s;
    demo.sample(s);
    Snapshot s2;
    demo.sample(s2);  // second sample still fine
    CHECK(s.cpu.usage_pct.has_value());
    CHECK(s.cpu.per_core_pct.size() == 16);
    CHECK(s.mem.ram_total_bytes.has_value());
    CHECK(s.gpus.size() == 2);
    CHECK(s2.gpus.size() == 2);
    finalize_gpus(s.gpus);
    CHECK(s.gpus[0].id == "gpu0");
    CHECK(s.gpus[1].id == "gpu1");
    CHECK(s.disk.devices.size() == 2);
    CHECK(s.net.interfaces.size() == 2);
    CHECK(s.gpus[1].is_integrated);  // the demo's AMD card is the Cezanne iGPU
    CHECK(s.gpus[1].integrated_method == "gpu_metrics format 2 (APU)");
    CHECK(!s.gpus[0].is_integrated);  // NVIDIA: always discrete
  }

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// AMD GPU: runtime-PM parsing, file formats, integrated-GPU decision
// ---------------------------------------------------------------------------

TEST_SUITE("gpu_amd") {
  TEST_CASE("parse_runtime_status maps every kernel value") {
    CHECK(parse_runtime_status("active") == RuntimeState::kActive);
    CHECK(parse_runtime_status("  resuming\n") == RuntimeState::kResuming);
    CHECK(parse_runtime_status("suspended") == RuntimeState::kSleeping);
    CHECK(parse_runtime_status("suspending") == RuntimeState::kSleeping);
    CHECK(parse_runtime_status("error") == RuntimeState::kError);
    // "unsupported" = runtime PM off = always awake, so it must read as
    // active, never as sleeping.
    CHECK(parse_runtime_status("unsupported") == RuntimeState::kUnsupported);
    // Unknown/empty text (failed read, vanished file): assume awake.
    CHECK(parse_runtime_status("") == RuntimeState::kActive);
    CHECK(parse_runtime_status("bogus") == RuntimeState::kActive);
  }

  TEST_CASE("parse_gpu_metrics_format reads format_revision from the header") {
    // struct metrics_table_header: u16 structure_size (LE), u8
    // format_revision, u8 content_revision. unsigned char: 0x80 does not
    // fit a signed char.
    const auto sv = [](const unsigned char* b, std::size_t n) {
      return std::string_view(reinterpret_cast<const char*>(b), n);
    };
    const unsigned char dpu[4] = {0x60, 0x00, 0x01, 0x02};  // size 160, format 1
    CHECK(parse_gpu_metrics_format(sv(dpu, 4)).value() == 1);
    const unsigned char apu[4] = {0x80, 0x00, 0x02, 0x02};  // size 208, format 2 (Cezanne)
    CHECK(parse_gpu_metrics_format(sv(apu, 4)).value() == 2);
    const unsigned char apu3[4] = {0x80, 0x00, 0x03, 0x03};
    CHECK(parse_gpu_metrics_format(sv(apu3, 4)).value() == 3);
    CHECK(parse_gpu_metrics_format(sv(apu, 3)).has_value() == false);  // too short
    // structure_size < 4: not a metrics table.
    const unsigned char tiny[4] = {0x02, 0x00, 0x01, 0x01};
    CHECK(parse_gpu_metrics_format(sv(tiny, 4)).has_value() == false);
  }

  TEST_CASE("parse_pp_dpm_mhz picks the '*' level in any unit") {
    const std::string sclk = "0: 200Mhz\n1: 400Mhz *\n2: 1800Mhz\n";
    CHECK(parse_pp_dpm_mhz(sclk).value() == 400.0);
    CHECK(parse_pp_dpm_mhz("0: 1000Khz *\n").value() == 1.0);
    CHECK(parse_pp_dpm_mhz("0: 1Ghz *\n").value() == 1000.0);
    CHECK(parse_pp_dpm_mhz("0: 200Mhz\n1: 400Mhz\n").has_value() == false);  // no '*'
    CHECK(parse_pp_dpm_mhz("").has_value() == false);
  }

  TEST_CASE("decide_amd_integrated: gpu_metrics is authoritative") {
    // format 1 = dGPU, and it wins over all the APU-looking fallbacks.
    const double kMiB = 1024.0 * 1024.0;
    auto d = decide_amd_integrated(1, 0x1638, 512.0 * kMiB, /*boot_vga=*/true,
                                   /*multi_gpu=*/true);
    CHECK(d.method == AmdIntegratedMethod::kGpuMetrics);
    CHECK(!d.integrated);

    d = decide_amd_integrated(2, 0x1638, 512.0 * kMiB, /*boot_vga=*/false,
                              /*multi_gpu=*/true);
    CHECK(d.method == AmdIntegratedMethod::kGpuMetrics);
    CHECK(d.integrated);  // format 2 = APU

    // format 0 / 4+ are not defined: fall through to the next signal.
    d = decide_amd_integrated(0, 0x1638, 8.0 * 1024.0 * kMiB, false, true);
    CHECK(d.method == AmdIntegratedMethod::kApuDeviceId);
  }

  TEST_CASE("decide_amd_integrated: APU device id is the second signal") {
    const double kMiB = 1024.0 * 1024.0;
    // No metrics read (device suspended at probe): the Cezanne id decides.
    auto d = decide_amd_integrated(std::nullopt, 0x1638, 8.0 * 1024.0 * kMiB,
                                   /*boot_vga=*/false, /*multi_gpu=*/true);
    CHECK(d.method == AmdIntegratedMethod::kApuDeviceId);
    CHECK(d.integrated);
    // A discrete card id is not on the list.
    d = decide_amd_integrated(std::nullopt, 0x7340, 8.0 * 1024.0 * kMiB, false,
                              true);
    CHECK(d.method == AmdIntegratedMethod::kDefaultDiscrete);
    CHECK(!d.integrated);
  }

  TEST_CASE("decide_amd_integrated: small VRAM implies shared system memory") {
    const double kMiB = 1024.0 * 1024.0;
    auto d = decide_amd_integrated(std::nullopt, 0x7340, 1.0 * 1024.0 * kMiB,
                                   false, true);
    CHECK(d.method == AmdIntegratedMethod::kSmallVram);
    CHECK(d.integrated);
    // Boundary: exactly 2 GiB still counts.
    d = decide_amd_integrated(std::nullopt, 0x7340, 2.0 * 1024.0 * kMiB, false,
                              true);
    CHECK(d.method == AmdIntegratedMethod::kSmallVram);
    // 8 GiB: too much for an APU slice.
    d = decide_amd_integrated(std::nullopt, 0x7340, 8.0 * 1024.0 * kMiB, false,
                              false);
    CHECK(d.method == AmdIntegratedMethod::kDefaultDiscrete);
  }

  TEST_CASE("decide_amd_integrated: boot_vga is only a last-resort tiebreaker") {
    const double kMiB = 1024.0 * 1024.0;
    auto d = decide_amd_integrated(std::nullopt, 0x7340, 8.0 * 1024.0 * kMiB,
                                   /*boot_vga=*/true, /*multi_gpu=*/true);
    CHECK(d.method == AmdIntegratedMethod::kBootVgaTiebreak);
    CHECK(d.integrated);
    // Single-GPU machine: there is no tie to break.
    d = decide_amd_integrated(std::nullopt, 0x7340, 8.0 * 1024.0 * kMiB, true,
                              /*multi_gpu=*/false);
    CHECK(d.method == AmdIntegratedMethod::kDefaultDiscrete);
    CHECK(!d.integrated);
  }

  TEST_CASE("kApuDeviceIds contains the known APU iGPUs") {
    const auto& ids = kApuDeviceIds();
    const auto has = [&](unsigned id) {
      return std::find(ids.begin(), ids.end(), id) != ids.end();
    };
    CHECK(has(0x1638));  // Cezanne
    CHECK(has(0x1636));  // Renoir
    CHECK(has(0x164d));  // Rembrandt
    CHECK(has(0x150e));  // Strix
    CHECK(!has(0x7340));  // an ordinary discrete id
  }

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------------

TEST_SUITE("format") {
  TEST_CASE("bytes uses binary units") {
    CHECK(format::bytes(1024.0) == "1.00 KiB");
    CHECK(format::bytes(1536.0) == "1.50 KiB");
    CHECK(format::bytes(1024.0 * 1024.0 * 1024.0 * 2.5) == "2.50 GiB");
    CHECK(format::bytes(500.0) == "500 B");
    CHECK(format::rate(1024.0 * 3) == "3.00 KiB/s");
  }

  TEST_CASE("freq switches to GHz above 1000 MHz") {
    CHECK(format::freq(800.0) == "800 MHz");
    CHECK(format::freq(1850.0) == "1.85 GHz");
  }

  TEST_CASE("or_na prints n/a for missing values") {
    std::optional<double> none;
    std::optional<double> some(12.0);
    CHECK(format::or_na(none, format::percent) == "n/a");
    CHECK(format::or_na(some, format::percent) == "12.0 %");
  }

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// PCI discovery + GPU registry (over a fake sysfs tree)
// ---------------------------------------------------------------------------


TEST_SUITE("pci_discovery") {
  TEST_CASE("finds class-0x03 devices with ids, driver, boot_vga, drm card") {
    test::FakePciTree tree;
    tree.add_device("0000:01:00.0", "10de", "25a2", "nvidia", false, "active", 0);
    tree.add_device("0000:02:00.0", "1002", "1638", "amdgpu", true, "active", 1);
    tree.add_device("0000:03:00.0", "8086", "9a49", "i915", false, "suspended", -1);
    // A non-display (network) class must be ignored.
    tree.write_dev_file("0000:04:00.0", "class", "0x020000\n");
    tree.write_dev_file("0000:04:00.0", "vendor", "0x8086\n");

    const auto devs = discover_pci_gpus(tree.root.string(),
                                        (tree.root / "class/drm").string());
    CHECK(devs.size() == 3);
    CHECK(devs[0].bus_id == "0000:01:00.0");
    CHECK(devs[0].vendor_id == 0x10de);
    CHECK(devs[0].device_id == 0x25a2);
    CHECK(devs[0].driver == "nvidia");
    CHECK(devs[0].drm_card == 0);
    CHECK(!devs[0].boot_vga);
    CHECK(devs[1].bus_id == "0000:02:00.0");
    CHECK(devs[1].driver == "amdgpu");
    CHECK(devs[1].boot_vga);
    CHECK(devs[1].drm_card == 1);
    CHECK(devs[2].bus_id == "0000:03:00.0");
    CHECK(devs[2].driver == "i915");
    CHECK(devs[2].drm_card == -1);
    CHECK(devs[2].runtime_status == "suspended");
  }

  // The spec's required test: a fake tree containing an Intel device that no
  // backend claims. It must still be reported, not dropped.
  TEST_CASE("registry keeps an unclaimed Intel device as 'no backend yet'") {
    test::FakePciTree tree;
    tree.add_device("0000:01:00.0", "1002", "1638", "amdgpu", true, "active", 0);
    tree.add_device("0000:02:00.0", "8086", "9a49", "i915", false, "active", 1);

    GpuRegistrySource reg;
    reg.set_sysfs_roots(tree.root.string(), (tree.root / "class/drm").string());
    reg.set_pci_ids_path(std::string(GETH_TEST_FIXTURES) + "/pci_ids.txt");
    const auto st = reg.probe();
    DOCTEST_REQUIRE(st.available);
    CHECK(reg.devices().size() == 2);
    // The i915 device is claimed by neither backend.
    CHECK(reg.unclaimed().size() == 1);
    DOCTEST_REQUIRE(!reg.unclaimed().empty());
    CHECK(reg.unclaimed()[0]->bus_id == "0000:02:00.0");
    CHECK(reg.unclaimed()[0]->driver == "i915");
    // The --probe text says so in words.
    CHECK(st.detail.find("no backend yet") != std::string::npos);
    // The claimed AMD device is NOT in the unclaimed list.
    CHECK(reg.unclaimed()[0]->bus_id != "0000:01:00.0");
  }

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// NVML strict bus-id matching (pure functions)
// ---------------------------------------------------------------------------

TEST_SUITE("nvml_match") {
  TEST_CASE("normalize_pci_bus_id: exact DDDD:BB:DD.F form, uppercased") {
    CHECK(normalize_pci_bus_id("0000:01:00.0") == "0000:01:00.0");
    CHECK(normalize_pci_bus_id(" 0000:ab:cd.e \n") == "0000:AB:CD.E");
    CHECK(normalize_pci_bus_id("01:00.0").has_value() == false);         // no domain
    CHECK(normalize_pci_bus_id("0000:1:00.0").has_value() == false);     // short bus
    CHECK(normalize_pci_bus_id("0000:01:00.12").has_value() == false);   // bad fn
    CHECK(normalize_pci_bus_id("0000:01:0g.0").has_value() == false);    // non-hex
    CHECK(normalize_pci_bus_id("").has_value() == false);
  }

  // The spec's required test: two NVIDIA GPUs reported by NVML in REVERSED
  // order must still pair with the right sysfs device by bus id — a
  // positional fallback would pair them wrongly.
  TEST_CASE("reversed NVML order matches by bus id, never by position") {
    const auto m =
        match_nvml_devices({"0000:01:00.0", "0000:03:00.0"},
                           {"0000:03:00.0", "0000:01:00.0"});
    CHECK(m.sysfs_unmatched.empty());
    CHECK(m.nvml_unmatched.empty());
    DOCTEST_REQUIRE(m.matched.size() == 2);
    CHECK(m.matched[0] == (std::pair<int, int>{0, 1}));  // sysfs[0] <-> nvml[1]
    CHECK(m.matched[1] == (std::pair<int, int>{1, 0}));  // sysfs[1] <-> nvml[0]
  }

  TEST_CASE("mismatches are reported in both directions, never guessed") {
    const auto m =
        match_nvml_devices({"0000:01:00.0", "0000:05:00.0"},
                           {"0000:01:00.0", "0000:09:00.0"});
    DOCTEST_REQUIRE(m.matched.size() == 1);
    CHECK(m.matched[0] == (std::pair<int, int>{0, 0}));
    CHECK(m.sysfs_unmatched == (std::vector<int>{1}));
    CHECK(m.nvml_unmatched == (std::vector<std::string>{"0000:09:00.0"}));
  }

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// NVML lazy session + suspend lifecycle (fake API + scripted runtime reader)
// ---------------------------------------------------------------------------

namespace {

struct NvmlCounters {
  int init = 0, shutdown = 0, get_count = 0, get_handle = 0, get_pci = 0,
      get_util = 0, get_mem = 0, get_clock = 0, get_power = 0, get_temp = 0;
};
NvmlCounters g_nvml;

int f_nvml_init() { return ++g_nvml.init, 0; }
void f_nvml_shutdown() { ++g_nvml.shutdown; }
int f_nvml_get_count(unsigned* c) { return ++g_nvml.get_count, *c = 1, 0; }
int f_nvml_get_handle(unsigned idx, void** h) {
  return ++g_nvml.get_handle, *h = reinterpret_cast<void*>(idx + 1), 0;
}
int f_nvml_get_pci(void*, void* buf) {
  return ++g_nvml.get_pci, std::snprintf(static_cast<char*>(buf), 32, "0000:01:00.0"),
         0;
}
int f_nvml_get_util(void*, unsigned* g, unsigned*) { return ++g_nvml.get_util, *g = 42, 0; }
int f_nvml_get_mem(void*, unsigned long long* t, unsigned long long* f,
                   unsigned long long* u) {
  return ++g_nvml.get_mem, *t = 100, *f = 50, *u = 50, 0;
}
int f_nvml_get_clock(void*, int, unsigned* c) { return ++g_nvml.get_clock, *c = 1500, 0; }
int f_nvml_get_power(void*, int* mw) { return ++g_nvml.get_power, *mw = 55000, 0; }
int f_nvml_get_temp(void*, int, unsigned* t) { return ++g_nvml.get_temp, *t = 47, 0; }

NvmlApi fake_nvml_api() {
  NvmlApi a;
  a.init = &f_nvml_init;
  a.shutdown = &f_nvml_shutdown;
  a.get_count = &f_nvml_get_count;
  a.get_handle = &f_nvml_get_handle;
  a.get_pci = &f_nvml_get_pci;
  a.get_util = &f_nvml_get_util;
  a.get_mem = &f_nvml_get_mem;
  a.get_clock = &f_nvml_get_clock;
  a.get_power = &f_nvml_get_power;
  a.get_temp = &f_nvml_get_temp;
  return a;
}

PciDevice fake_nvidia_dev() {
  PciDevice d;
  d.bus_id = "0000:01:00.0";
  d.vendor_id = 0x10de;
  d.driver = "nvidia";
  return d;
}

}  // namespace

TEST_SUITE("nvml_lifecycle") {
  // The spec's required sleep-path test: active -> suspended (x12) -> active.
  // Assert: no NVML sample calls while suspended; nvmlShutdown after 10
  // suspended ticks; NVML re-initialises on the next active tick.
  TEST_CASE("suspend lifecycle: no queries asleep, shutdown after 10, re-init awake") {
    PciIds ids;  // never loaded; names fall back
    GpuNvidiaBackend be(ids);
    const PciDevice dev = fake_nvidia_dev();
    be.attach(dev);
    be.set_fake_api(fake_nvml_api());
    std::string state = "active";
    be.set_runtime_reader([&](const PciDevice&) { return state; });
    g_nvml = NvmlCounters{};

    // Tick 1: active -> lazy init, then queries.
    be.begin_tick();
    {
      GpuSample g;
      be.sample(dev, g);
      CHECK(g.state == GpuState::kActive);
      CHECK(g.usage_pct.has_value());
    }
    CHECK(g_nvml.init == 1);
    CHECK(g_nvml.get_util == 1);
    const int util_t1 = g_nvml.get_util;  // baseline: one awake tick's queries
    const int mem_t1 = g_nvml.get_mem;

    // Ticks 2..13: suspended x12 -> zero NVML sample calls.
    state = "suspended";
    for (int i = 0; i < 12; ++i) {
      be.begin_tick();
      GpuSample g;
      be.sample(dev, g);
      CHECK(g.state == GpuState::kSleeping);
      CHECK(g.usage_pct.has_value() == false);
    }
    CHECK(g_nvml.get_util == util_t1);  // no sample call while suspended
    CHECK(g_nvml.get_mem == mem_t1);
    CHECK(g_nvml.init == 1);            // not re-opened while suspended
    CHECK(g_nvml.shutdown == 1);        // closed after 10 consecutive suspended ticks

    // Tick 14: active again -> re-initialise, queries resume.
    state = "active";
    be.begin_tick();
    {
      GpuSample g;
      be.sample(dev, g);
      CHECK(g.state == GpuState::kActive);
      CHECK(g.usage_pct.has_value());
    }
    CHECK(g_nvml.init == 2);       // re-initialised on the next active tick
    CHECK(g_nvml.get_util == 2);
  }

  TEST_CASE("a few suspended ticks (< 10) do NOT close the session") {
    PciIds ids;
    GpuNvidiaBackend be(ids);
    const PciDevice dev = fake_nvidia_dev();
    be.attach(dev);
    be.set_fake_api(fake_nvml_api());
    std::string state = "active";
    be.set_runtime_reader([&](const PciDevice&) { return state; });
    g_nvml = NvmlCounters{};

    // Wake the session once.
    be.begin_tick();
    GpuSample g0;
    be.sample(dev, g0);
    CHECK(g_nvml.init == 1);

    // 9 suspended ticks: the streak reaches 9 but never the shutdown threshold.
    state = "suspended";
    for (int i = 0; i < 9; ++i) {
      be.begin_tick();
      GpuSample g;
      be.sample(dev, g);
      CHECK(g.state == GpuState::kSleeping);
    }
    CHECK(g_nvml.shutdown == 0);  // still open: only 9 consecutive ticks
    CHECK(g_nvml.init == 1);
  }

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// AMD APU vs discrete power split (over a fake sysfs tree)
// ---------------------------------------------------------------------------

TEST_SUITE("amd_power_split") {
  // Helper: build an amdgpu device with a PPT (power1_input, microwatts) and
  // a VRAM total, then attach + sample it through the backend.
  GpuSample sample_amd(const std::string& device_id, unsigned long long vram_bytes,
                       const std::string& ppt_microwatts = "47000000") {
    test::FakePciTree tree;
    const std::string bus = "0000:07:00.0";
    tree.add_device(bus, "1002", device_id, "amdgpu", true, "active", 0);
    const auto dev = tree.root / "bus/pci/devices" / bus;
    const auto hwmon = dev / "hwmon/hwmon0";
    std::filesystem::create_directories(hwmon);
    { std::ofstream f(hwmon / "name"); f << "amdgpu\n"; }
    { std::ofstream f(hwmon / "power1_input"); f << ppt_microwatts << "\n"; }
    { std::ofstream f(dev / "gpu_busy_percent"); f << "3\n"; }
    { std::ofstream f(dev / "mem_info_vram_total"); f << vram_bytes << "\n"; }
    { std::ofstream f(dev / "mem_info_vram_used"); f << "104857600\n"; }

    PciIds ids;  // never loaded; names fall back
    GpuAmdBackend be(ids);
    PciDevice pd;
    pd.bus_id = bus;
    pd.dev_path = dev.string();
    pd.vendor_id = 0x1002;
    pd.device_id = std::stoul(device_id, nullptr, 16);
    pd.driver = "amdgpu";
    pd.boot_vga = true;
    be.attach(pd);
    GpuSample g;
    be.sample(pd, g);
    return g;
  }

  TEST_CASE("integrated APU iGPU: raw PPT kept, power_w stays nullopt") {
    // 0x1638 = Cezanne (a known APU id); 512 MiB VRAM (shared).
    const auto g = sample_amd("1638", 512ull * 1024 * 1024);
    CHECK(g.is_integrated);
    CHECK(g.state == GpuState::kActive);
    CHECK(g.ppt_w.has_value());
    CHECK(g.ppt_w.value() == 47.0);      // raw value kept for --probe/debug
    CHECK(g.power_w.has_value() == false);  // PPT is package power, not card power
  }

  TEST_CASE("discrete AMD dGPU: power_w equals the PPT reading") {
    // 0x7340 is not a known APU id; 8 GiB VRAM -> discrete.
    const auto g = sample_amd("7340", 8ull * 1024 * 1024 * 1024);
    CHECK(g.is_integrated == false);
    CHECK(g.ppt_w.has_value());
    CHECK(g.power_w.has_value());
    CHECK(g.power_w.value() == 47.0);  // discrete: PPT is the card's power
  }

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// APU package power: the Collector lifts the integrated GPU's PPT into the
// CPU package power when no counter source won.
// ---------------------------------------------------------------------------

namespace {

// Emits one active integrated AMD GPU with a PPT reading (and, optionally, a
// pre-set CPU package power to simulate a counter source having won).
struct FakeApuGpuSource : Source {
  std::optional<double> preset_cpu_power;  // set => a counter source already won
  bool gpu_active = true;
  double ppt = 47.0;  // the PPT reading; the regression tests vary it per tick

  const char* name() const override { return "fake apu gpu"; }
  SourceStatus probe() override {
    SourceStatus st;
    st.available = true;
    return st;
  }
  void sample(Snapshot& out) override {
    if (preset_cpu_power) out.cpu.package_power_w = preset_cpu_power;
    // The Collector reuses one Snapshot, so fill in place (identity once).
    if (out.gpus.size() != 1) out.gpus.resize(1);
    GpuSample& g = out.gpus[0];
    if (g.pci_bus_id.empty()) {
      g.vendor = GpuVendor::kAmd;
      g.name = "Fake iGPU";
      g.pci_bus_id = "0000:07:00.0";
      g.is_integrated = true;
    }
    g.state = gpu_active ? GpuState::kActive : GpuState::kSleeping;
    g.ppt_w = gpu_active ? std::optional<double>(ppt) : std::nullopt;
    g.power_w = std::nullopt;  // the fake card reports no card power
  }
};

}  // namespace

TEST_SUITE("apu_package_power") {
  TEST_CASE("active iGPU PPT becomes CPU package power when no counter won") {
    Collector c;
    c.add(std::make_unique<FakeApuGpuSource>());
    const auto s = c.sample();
    CHECK(s.cpu.package_power_w.has_value());
    CHECK(s.cpu.package_power_w.value() == 47.0);
    CHECK(s.cpu.package_power_source == CpuPackagePowerSource::kApuPpt);
    // The card's own power stays nullopt (PPT is package power, not card's).
    CHECK(s.gpus[0].power_w.has_value() == false);
    CHECK(s.gpus[0].ppt_w.value() == 47.0);
  }

  TEST_CASE("a counter source's value is never overwritten by APU PPT") {
    Collector c;
    auto src = std::make_unique<FakeApuGpuSource>();
    src->preset_cpu_power = 30.0;  // simulates RAPL/amd_energy/zenpower winning
    c.add(std::move(src));
    const auto s = c.sample();
    CHECK(s.cpu.package_power_w.value() == 30.0);  // untouched
  }

  TEST_CASE("a suspended iGPU does not supply CPU package power") {
    Collector c;
    auto src = std::make_unique<FakeApuGpuSource>();
    src->gpu_active = false;
    c.add(std::move(src));
    const auto s = c.sample();
    CHECK(s.cpu.package_power_w.has_value() == false);
    CHECK(s.cpu.package_power_source.has_value() == false);
  }

  // The full startup chain from main.cpp — the regression this test exists
  // for. Phase 2.1's probe_all() never lifted the PPT, so
  // hardware_from(collector.latest()) saw no package power and
  // build_default_page() dropped the CPU line from the Power panel on
  // machines where RAPL is unreadable (EACCES). The cases above call
  // sample() directly, which always lifts — so they could not catch it.
  TEST_CASE("regression: probe_all -> hardware_from -> build_default_page keeps the APU PPT CPU line") {
    Collector c;
    c.add(std::make_unique<FakeApuGpuSource>());  // no preset: no counter won
    c.probe_all(/*warm_up=*/true);
    const ui::Hardware hw = ui::hardware_from(c.latest(), "Fake APU");
    CHECK_EQ(hw.cpu_power_label, std::string("APU Package (PPT)"));

    const std::vector<ui::PanelSpec> page = ui::build_default_page(hw);
    const ui::PanelSpec* power = nullptr;
    for (const auto& p : page)
      if (p.id == "power") power = &p;
    REQUIRE(power != nullptr);
    REQUIRE(!power->series.empty());
    // The CPU line is pushed before any GPU line; the fake iGPU never
    // reports card power, so it is also the only line.
    const ui::SeriesSpec& cpu_series = power->series.front();
    CHECK_EQ(cpu_series.label, std::string("APU Package (PPT)"));
    CHECK_EQ(cpu_series.value_fn(c.latest()), std::string("47.0 W"));
  }

  // v1.0.3 regression: the Collector reuses ONE Snapshot across ticks and
  // CpuPowerSource writes the power fields only when its backend produced a
  // value — on an APU-PPT machine it never does. The lift's "a counter source
  // already won" guard then read the value the LIFT itself stored on the
  // previous tick and skipped forever, freezing package power at the first
  // sample (the flat "APU 5.0 W" under full load). Every tick must lift the
  // CURRENT reading. All pre-existing cases above are single-tick, so none
  // of them could see it.
  TEST_CASE("regression: the lift follows a changing PPT on every tick") {
    Collector c;
    auto src = std::make_unique<FakeApuGpuSource>();
    FakeApuGpuSource* s = src.get();  // the object outlives the moved pointer
    c.add(std::move(src));
    for (int i = 0; i < 4; ++i) {
      s->ppt = 40.0 + i;  // the sensor climbs under load
      const auto snap = c.sample();
      REQUIRE(snap.cpu.package_power_w.has_value());
      CHECK_EQ(snap.cpu.package_power_w.value(), 40.0 + i);
      CHECK(snap.cpu.package_power_source == CpuPackagePowerSource::kApuPpt);
    }
  }

  TEST_CASE("regression: a woken iGPU updates the lifted power (no stale value)") {
    Collector c;
    auto src = std::make_unique<FakeApuGpuSource>();
    FakeApuGpuSource* s = src.get();
    s->ppt = 40.0;
    c.add(std::move(src));

    CHECK_EQ(c.sample().cpu.package_power_w.value(), 40.0);

    s->gpu_active = false;  // the iGPU sleeps (suspend)
    CHECK(c.sample().cpu.package_power_w.has_value() == false);  // no stale 40 W

    s->gpu_active = true;
    s->ppt = 42.0;  // wakes under load
    const auto s3 = c.sample();
    CHECK_EQ(s3.cpu.package_power_w.value(), 42.0);  // not the old 40 W
    CHECK(s3.cpu.package_power_source == CpuPackagePowerSource::kApuPpt);
  }

  // End-to-end through the REAL sources over a fake /sys: a CpuPowerSource
  // with no powercap/hwmon counter (the EACCES machine), a GpuRegistrySource
  // over a fake Cezanne APU, and a power1_input file rewritten between
  // ticks. The sampled value must follow the sensor every tick; pre-fix it
  // froze at the warm-up reading.
  TEST_CASE("regression: the lift follows a changing power1_input (fake sysfs)") {
    test::FakePciTree tree;
    const std::string bus = "0000:07:00.0";
    tree.add_device(bus, "1002", "1638", "amdgpu", true, "active", 0);
    tree.write_dev_file(bus, "hwmon/hwmon0/name", "amdgpu\n");
    tree.write_dev_file(bus, "hwmon/hwmon0/power1_input", "10000000\n");
    tree.write_dev_file(bus, "gpu_busy_percent", "3\n");
    tree.write_dev_file(bus, "mem_info_vram_total", "536870912\n");  // 512 MiB (shared)
    tree.write_dev_file(bus, "mem_info_vram_used", "104857600\n");

    Collector c;
    c.add(std::make_unique<CpuPowerSource>(tree.root.string()));
    auto reg = std::make_unique<GpuRegistrySource>();
    reg->set_sysfs_roots(tree.root.string(), (tree.root / "class/drm").string());
    reg->set_pci_ids_path(std::string(GETH_TEST_FIXTURES) + "/pci_ids.txt");
    c.add(std::move(reg));
    c.probe_all(/*warm_up=*/true);
    DOCTEST_REQUIRE(!c.latest().gpus.empty());
    CHECK(c.latest().gpus[0].is_integrated);  // 0x1638 is a known APU id

    const auto expect = [&](unsigned long long microwatts, double watts) {
      tree.write_dev_file(bus, "hwmon/hwmon0/power1_input",
                          std::to_string(microwatts) + "\n");
      const auto s = c.sample();
      REQUIRE(s.cpu.package_power_w.has_value());
      CHECK(s.cpu.package_power_source == CpuPackagePowerSource::kApuPpt);
      CHECK_EQ(s.cpu.package_power_w.value(), watts);
    };
    expect(30000000, 30.0);  // load ramps up
    expect(40000000, 40.0);
    expect(50000000, 50.0);  // pre-fix: still 10.0 W here
  }

  // Work order: when RAPL becomes readable (the udev rule / setcap installed),
  // the Power panel must switch from "APU Package (PPT)" to "CPU Package" —
  // a counter source beats the APU PPT lift. The live temp tree lets the
  // RAPL counter advance between samples (static fixture => 0 W).
  TEST_CASE("RAPL readable: it beats the APU PPT and relabels to 'CPU Package'") {
    const auto root = std::filesystem::temp_directory_path() /
        ("geth_rapl_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    const auto dev = root / "class/powercap/package-0";
    auto wf = [&](const char* name, const std::string& v) {
      std::filesystem::create_directories(dev);
      std::ofstream f(dev / name);
      f << v;
    };
    wf("name", "package-0");
    wf("energy_uj", "10000000");        // 10 mJ
    wf("max_energy_range_uj", "100000000");

    Collector c;
    c.add(std::make_unique<CpuPowerSource>(root.string()));
    c.add(std::make_unique<FakeApuGpuSource>());  // 47 W PPT — must lose
    c.probe_all(/*warm_up=*/true);  // RAPL baseline set here

    // Advance the RAPL counter by 1 mJ, ~100 ms later => ~10 W.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    wf("energy_uj", "11000000");
    const auto s = c.sample();
    DOCTEST_REQUIRE(s.cpu.package_power_w.has_value());
    CHECK(s.cpu.package_power_source == CpuPackagePowerSource::kRapl);
    CHECK(s.cpu.package_power_w.value() > 1.0);
    CHECK(s.cpu.package_power_w.value() < 100.0);
    CHECK(s.cpu.package_power_w.value() != 47.0);  // not the iGPU's PPT

    // The panel label switches with the source.
    const ui::Hardware hw = ui::hardware_from(c.latest(), "Fake APU");
    CHECK_EQ(hw.cpu_power_label, std::string("CPU Package"));
    const std::vector<ui::PanelSpec> page = ui::build_default_page(hw);
    const ui::PanelSpec* power = nullptr;
    for (const auto& p : page)
      if (p.id == "power") power = &p;
    REQUIRE(power != nullptr);
    REQUIRE(!power->series.empty());
    CHECK_EQ(power->series.front().label, std::string("CPU Package"));
    std::filesystem::remove_all(root);
  }

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// CPU package power sources (fixture sysfs trees under GETH_TEST_FIXTURES)
//
// The fixtures mirror the REAL driver layouts, not an assumed one:
//   sys_zenpower/     hwmon "zenpower"   — instantaneous power1/2_input (µW)
//   sys_amd_energy/   hwmon "amd_energy" — cumulative energyN_input (µJ),
//                     Esocket* + Ecore* labels
//   sys_rapl/         powercap "package-0" — energy_uj + max_energy_range_uj
//   sys_rapl_eacces/  powercap "package-0" — energy_uj mode 000 (root-only)
//   sys_bad/          hwmon "amd_energy" — ONLY a power1_raw file (wrong
//                     layout; the old, incorrect assumption) -> must reject
// ---------------------------------------------------------------------------

TEST_SUITE("cpu_power_sources") {
  const std::string fix = std::string(GETH_TEST_FIXTURES);

  TEST_CASE("zenpower: instantaneous core + SoC microwatts are summed") {
    CpuPowerSource src(fix + "/sys_zenpower");
    const auto st = src.probe();
    DOCTEST_REQUIRE(st.available);
    CHECK(st.detail.find("zenpower") != std::string::npos);
    CHECK(st.detail.find("SVI2_P_Core") != std::string::npos);

    Snapshot snap;
    src.sample(snap);  // instantaneous: no baseline needed
    DOCTEST_REQUIRE(snap.cpu.package_power_w.has_value());
    CHECK(snap.cpu.package_power_w.value() == 20.0);  // 15 W core + 5 W SoC
    CHECK(snap.cpu.package_power_source == CpuPackagePowerSource::kZenpower);
  }

  TEST_CASE("amd_energy: only Esocket* counters are summed, delta over time") {
    CpuPowerSource src(fix + "/sys_amd_energy");
    const auto st = src.probe();
    DOCTEST_REQUIRE(st.available);
    CHECK(st.detail.find("1 Esocket*") != std::string::npos);  // Ecore* ignored

    Snapshot base, second;
    src.sample(base);  // first sample: baseline only
    CHECK(base.cpu.package_power_w.has_value() == false);
    src.sample(second);  // static fixture: counter unchanged -> 0 W
    DOCTEST_REQUIRE(second.cpu.package_power_w.has_value());
    CHECK(second.cpu.package_power_w.value() == 0.0);
    CHECK(second.cpu.package_power_source == CpuPackagePowerSource::kAmdEnergy);
  }

  TEST_CASE("amd_energy: a real delta yields power over elapsed time") {
    // Live tree (temp dir) so the counter can advance between samples.
    const auto root = std::filesystem::temp_directory_path() /
        ("geth_amd_energy_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    const auto dev = root / "class/hwmon/hwmon9";
    auto wf = [&](const char* name, const std::string& v) {
      std::filesystem::create_directories(dev);
      std::ofstream f(dev / name);
      f << v;
    };
    wf("name", "amd_energy");
    wf("energy1_label", "Esocket0");
    wf("energy1_input", "1000000");   // 1 mJ
    wf("energy2_label", "Ecore000");   // must be ignored
    wf("energy2_input", "999");

    {
      CpuPowerSource src(root.string());
      const auto st = src.probe();
      DOCTEST_REQUIRE(st.available);
      CHECK(st.detail.find("1 Esocket*") != std::string::npos);

      Snapshot base;
      src.sample(base);  // baseline
      CHECK(base.cpu.package_power_w.has_value() == false);

      // Advance the Esocket counter by 1 mJ, ~100 ms later => ~10 W.
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      wf("energy1_input", "2000000");
      Snapshot next;
      src.sample(next);
      DOCTEST_REQUIRE(next.cpu.package_power_w.has_value());
      CHECK(next.cpu.package_power_w.value() > 1.0);
      CHECK(next.cpu.package_power_w.value() < 100.0);
    }
    std::filesystem::remove_all(root);
  }

  TEST_CASE("amd_energy: a backwards counter is a reset, skipped not wrapped") {
    const auto root = std::filesystem::temp_directory_path() /
        ("geth_amd_reset_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    const auto dev = root / "class/hwmon/hwmon9";
    auto wf = [&](const char* name, const std::string& v) {
      std::filesystem::create_directories(dev);
      std::ofstream f(dev / name);
      f << v;
    };
    wf("name", "amd_energy");
    wf("energy1_label", "Esocket0");
    wf("energy1_input", "1000000");

    {
      CpuPowerSource src(root.string());
      DOCTEST_REQUIRE(src.probe().available);
      Snapshot base;
      src.sample(base);  // baseline at 1,000,000

      // Counter RESETS to a smaller value (driver re-init) — not a wrap,
      // because amd_energy publishes no range. This tick must be skipped.
      wf("energy1_input", "50");
      Snapshot after_reset;
      src.sample(after_reset);
      CHECK(after_reset.cpu.package_power_w.has_value() == false);

      // The baseline re-anchored: the next delta is from 50, producing power.
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      wf("energy1_input", "500050");  // +500,000 µJ
      Snapshot recovered;
      src.sample(recovered);
      CHECK(recovered.cpu.package_power_w.has_value() == true);
      CHECK(recovered.cpu.package_power_w.value() > 0.0);
    }
    std::filesystem::remove_all(root);
  }

  TEST_CASE("RAPL: package domain is accepted, delta over time") {
    CpuPowerSource src(fix + "/sys_rapl");
    const auto st = src.probe();
    DOCTEST_REQUIRE(st.available);
    CHECK(st.detail.find("RAPL package domains: 1") != std::string::npos);

    Snapshot base, second;
    src.sample(base);
    CHECK(base.cpu.package_power_w.has_value() == false);
    src.sample(second);  // static: 0 W
    DOCTEST_REQUIRE(second.cpu.package_power_w.has_value());
    CHECK(second.cpu.package_power_w.value() == 0.0);
    CHECK(second.cpu.package_power_source == CpuPackagePowerSource::kRapl);
  }

  // Regression, from a real AMD Zen (Ryzen 7 260) box: the powercap class
  // directory is FLAT (zone + every domain symlinked at the top level) and
  // each domain's device directory contains a `device` symlink pointing at
  // its PARENT. The single package counter was therefore reachable through
  // three paths —
  //   class/powercap/intel-rapl:0/energy_uj          (the domain's own link)
  //   class/powercap/intel-rapl/intel-rapl:0/...     (the zone's child dir)
  //   class/powercap/intel-rapl:0:0/device/energy_uj (core's `device` -> pkg)
  // — and sample() summed all three deltas, tripling the reported power
  // (84.5 W read while the package drew ~28 W; the probe's RAPL-vs-PPT
  // cross-check showed 41.7 W vs 10.9 W). The tree below reproduces that
  // topology exactly; the counter must be counted ONCE.
  TEST_CASE("regression: the flat class dir + `device` links count a counter once") {
    const auto root = std::filesystem::temp_directory_path() /
        ("geth_rapl_dup_" + std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    const auto real = root / "devices/platform/intel-rapl";
    const auto pkg = real / "intel-rapl:0";
    const auto core = pkg / "intel-rapl:0:0";
    const auto pc = root / "class/powercap";
    std::filesystem::create_directories(core);
    std::filesystem::create_directories(pc);
    auto wf = [](const std::filesystem::path& f, const std::string& v) {
      std::ofstream o(f);
      o << v;
    };
    // The real device tree (the zone has no package name of its own).
    wf(pkg / "name", "package-0");
    wf(pkg / "energy_uj", "10000000");
    wf(pkg / "max_energy_range_uj", "100000000");
    wf(core / "name", "core");
    wf(core / "energy_uj", "42");
    wf(core / "max_energy_range_uj", "100000000");
    // Kernel `device` symlinks: each points at the PARENT device dir.
    std::error_code ec;
    std::filesystem::create_symlink("../../intel-rapl", pkg / "device", ec);
    std::filesystem::create_symlink("../../intel-rapl:0", core / "device", ec);
    // The flat class dir: zone, package domain and core domain, all linked
    // at the top level.
    std::filesystem::create_symlink(real, pc / "intel-rapl", ec);
    std::filesystem::create_symlink(pkg, pc / "intel-rapl:0", ec);
    std::filesystem::create_symlink(core, pc / "intel-rapl:0:0", ec);

    CpuPowerSource src(root.string());
    const auto st = src.probe();
    DOCTEST_REQUIRE(st.available);
    CHECK(st.detail.find("RAPL package domains: 1") != std::string::npos);  // was 3
    REQUIRE(st.paths.size() == 1);

    // The counted delta must be the package counter's, not three copies of
    // it: +1 mJ over ~100 ms => ~10 W (pre-fix: ~30 W).
    Snapshot base;
    src.sample(base);  // baseline
    CHECK(base.cpu.package_power_w.has_value() == false);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::ofstream(pkg / "energy_uj") << "11000000\n";
    Snapshot next;
    src.sample(next);
    DOCTEST_REQUIRE(next.cpu.package_power_w.has_value());
    CHECK(next.cpu.package_power_w.value() > 1.0);
    CHECK(next.cpu.package_power_w.value() < 20.0);  // 3x (~30 W) fails this
    std::filesystem::remove_all(root);
  }

  TEST_CASE("RAPL: EACCES reports unavailable with CapEff and the udev fix") {
    // The fixture's energy_uj ships world-readable (git cannot store a 0000
    // mode), so force it unreadable here to simulate a non-root process facing
    // a root-only RAPL counter; the original mode is restored afterwards. As
    // root, file modes cannot be used to deny access, so the EACCES path is
    // not reproducible and the case is a documented no-op.
    const std::string uj =
        fix + "/sys_rapl_eacces/class/powercap/intel-rapl:0/energy_uj";
    if (::geteuid() == 0) return;  // root ignores file modes
    struct stat stb{};
    const auto old_mode = ::stat(uj.c_str(), &stb) == 0
                              ? static_cast<mode_t>(stb.st_mode & 07777)
                              : 0644;
    ::chmod(uj.c_str(), 0);
    CpuPowerSource src(fix + "/sys_rapl_eacces");
    const auto st = src.probe();
    ::chmod(uj.c_str(), old_mode);
    CHECK(st.available == false);
    CHECK(st.detail.find("EACCES") != std::string::npos);
    CHECK(st.detail.find("CapEff: 0x") != std::string::npos);
    // The hint names both CMake fix targets (udev rule and setcap).
    CHECK(st.hint.find("install-udev") != std::string::npos);
    CHECK(st.hint.find("setcap") != std::string::npos);
  }

  TEST_CASE("a hwmon dir with only power1_raw is NOT accepted") {
    // The layout some tools wrongly assume. Neither real driver exposes
    // power1_raw, so a dir that has ONLY it must be rejected, not adopted.
    CpuPowerSource src(fix + "/sys_bad");
    const auto st = src.probe();
    CHECK(st.available == false);
    Snapshot snap;
    src.sample(snap);
    CHECK(snap.cpu.package_power_w.has_value() == false);
  }

}  // TEST_SUITE

// ---------------------------------------------------------------------------
// NVML at startup (A5): the brief enumeration session is a --probe-only
// feature. At UI/dump startup it must never run — even one nvmlInit can keep
// the driver (and the GPU) awake.
// ---------------------------------------------------------------------------

namespace {

int g_nvml_init_calls = 0;
int g_nvml_shutdown_calls = 0;

int counting_nvml_init() { return ++g_nvml_init_calls, 0; }
void counting_nvml_shutdown() { ++g_nvml_shutdown_calls; }
int counting_nvml_get_count(unsigned* c) { *c = 0; return 0; }  // no NVML devices
int counting_nvml_get_handle(unsigned, void**) { return 1; }  // never called (count 0)

NvmlApi counting_nvml_api() {
  NvmlApi api{};
  api.init = counting_nvml_init;
  api.shutdown = counting_nvml_shutdown;
  api.get_count = counting_nvml_get_count;
  api.get_handle = counting_nvml_get_handle;  // must be non-null to pass the ABI check
  return api;
}

}  // namespace

TEST_SUITE("nvml_at_startup") {
  TEST_CASE("UI/dump startup never calls nvmlInit while suspended; --probe does") {
    g_nvml_init_calls = 0;
    g_nvml_shutdown_calls = 0;

    test::FakePciTree tree;
    tree.add_device("0000:01:00.0", "10de", "25a2", "nvidia", false, "suspended", 0);

    GpuRegistrySource reg;
    reg.set_sysfs_roots(tree.root.string(), (tree.root / "class/drm").string());
    reg.set_pci_ids_path(std::string(GETH_TEST_FIXTURES) + "/pci_ids.txt");
    GpuNvidiaBackend* nv = reg.nvidia_backend();
    DOCTEST_REQUIRE(nv != nullptr);
    nv->set_fake_api(counting_nvml_api());

    // --- UI/dump startup: probe + a sample while the card is suspended ---
    reg.probe();  // default: non-probe mode
    CHECK(g_nvml_init_calls == 0);
    Snapshot snap;
    reg.sample(snap);
    CHECK(g_nvml_init_calls == 0);  // still no session: the card is asleep

    // --- --probe startup: the brief enumeration runs (init + shutdown) ---
    reg.set_probe_mode(true);
    reg.probe();
    CHECK(g_nvml_init_calls == 1);
    CHECK(g_nvml_shutdown_calls == 1);  // and it is closed again
  }
}  // TEST_SUITE
