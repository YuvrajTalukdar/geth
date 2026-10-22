// v1.5.0 — the CSV data logger: column selection (visible panels minus
// hidden series, de-duplicated by metric), unique header names, value
// precision and locale independence, RFC 4180 quoting, rotation on page
// change, session files, error handling, the GETH_TEST guard, and the
// zero-heap-allocation guarantee per logged row.

#include <doctest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <clocale>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/snapshot.hpp"
#include "ui/csv_log.hpp"
#include "ui/panel_spec.hpp"
#include "util/alloc_count.hpp"

#include "ui_test_util.hpp"

// glibc (the only platform GETH targets); the offset check interprets a
// broken-down local time as UTC.
extern "C" std::time_t timegm(std::tm*);

using namespace geth;

namespace {

// The demo hardware, by hand (the same identity as sources/demo.cpp): one
// NVIDIA dGPU and one AMD iGPU (an APU, hence "APU Package (PPT)").
ui::Hardware demo_hw() {
  ui::Hardware h;
  h.cpu_name = "AMD Ryzen 5 5600H with Radeon Graphics";
  h.ram_total_bytes = 32.0 * 1073741824.0;
  // Mirror hardware_from(): the APU package power source is what makes the
  // Power panel carry the CPU line (and the log name's "<source>").
  h.cpu_power_source = CpuPackagePowerSource::kApuPpt;
  h.cpu_power_label = "APU Package (PPT)";
  ui::GpuInfo d;
  d.bus_id = "0000:01:00.0";
  d.name = "NVIDIA GeForce RTX 3050 Laptop GPU";
  d.vendor = GpuVendor::kNvidia;
  d.is_integrated = false;
  d.vram_total_bytes = 8.0 * 1073741824.0;
  d.reports_power = true;
  ui::GpuInfo a;
  a.bus_id = "0000:07:00.0";
  a.name = "Cezanne [Radeon Vega Series / Radeon Vega Mobile Series]";
  a.vendor = GpuVendor::kAmd;
  a.is_integrated = true;
  a.vram_total_bytes = 1.0 * 1073741824.0;
  a.gtt_total_bytes = 0.5 * 1073741824.0;
  h.gpus = {d, a};
  return h;
}

// One sample with every loggable field set to a value that exercises the
// precision rules — deliberately away from .5 boundaries — plus one missing
// field (swap) and one NaN (the iGPU core clock, the suspend-gap case).
Snapshot mk_snap(TimePoint t) {
  Snapshot s;
  s.time = t;
  s.cpu.usage_pct = 42.56;           // %  -> "42.6"
  s.cpu.freq_avg_mhz = 2399.6;       // MHz -> "2400"
  s.cpu.freq_max_mhz = 2800.4;       // MHz -> "2800"
  s.cpu.temp_c = 55.04;              // C   -> "55.0"
  s.cpu.package_power_w = 12.352;    // W   -> "12.35"
  s.cpu.package_power_source = CpuPackagePowerSource::kApuPpt;
  s.mem.ram_total_bytes = 32.0 * 1073741824.0;
  s.mem.ram_used_bytes = 10737418240.0;  // B   -> "10737418240"
  s.mem.swap_total_bytes = 2.0 * 1073741824.0;
  s.mem.swap_used_bytes = std::nullopt;  // missing -> EMPTY field
  GpuSample d;
  d.id = "gpu0";
  d.pci_bus_id = "0000:01:00.0";
  d.is_integrated = false;
  d.vendor = GpuVendor::kNvidia;
  d.usage_pct = 77.56;             // %   -> "77.6"
  d.vram_used_bytes = 5905580646.0;  // B   -> "5905580646"
  d.core_clock_mhz = 1650.4;       // MHz -> "1650"
  d.mem_clock_mhz = 1750.0;        // MHz -> "1750"
  d.power_w = 28.004;              // W   -> "28.00"
  d.temp_c = 47.02;                // C   -> "47.0"
  GpuSample a;
  a.id = "gpu1";
  a.pci_bus_id = "0000:07:00.0";
  a.is_integrated = true;
  a.vendor = GpuVendor::kAmd;
  a.usage_pct = 12.34;           // %   -> "12.3"
  a.vram_used_bytes = 429496729.6;  // B   -> "429496730"
  a.gtt_used_bytes = 322122547.2;   // B   -> "322122547"
  a.core_clock_mhz = std::nan("");  // NaN -> EMPTY field
  a.mem_clock_mhz = 1600.0;        // MHz -> "1600"
  a.temp_c = 41.96;                // C   -> "42.0"
  s.gpus = {d, a};
  s.net.total_rx_bytes_per_s = 1048576.4;  // B/s -> "1048576"
  s.net.total_tx_bytes_per_s = 524288.6;   // B/s -> "524289"
  s.disk.total_read_bytes_per_s = 4194304.2;   // B/s -> "4194304"
  s.disk.total_write_bytes_per_s = 1048577.7;  // B/s -> "1048578"
  return s;
}

// The machine's UTC offset in minutes at `now` — computed independently of
// the logger's civil-math: mktime interprets the local wall fields as local
// time, timegm as UTC; the difference IS the offset.
long long machine_offset_min() {
  const std::time_t now = std::time(nullptr);
  struct tm lt{};
  struct tm cp{};
  localtime_r(&now, &lt);
  cp = lt;
  const std::time_t as_local = ::mktime(&cp);
  const std::time_t as_utc = ::timegm(&lt);
  return static_cast<long long>(as_local - as_utc) / 60;
}

}  // namespace

TEST_SUITE("csv_log") {

TEST_CASE("columns: the default page logs every visible value, de-dup'd, unique headers") {
  const auto hw = demo_hw();
  const auto page = ui::build_default_page(hw);
  const std::string dn = ui::gpu_short_name(hw.gpus[0]);  // "RTX 3050 Laptop"
  const std::string an = ui::gpu_short_name(hw.gpus[1]);  // "Cezanne iGPU"

  const auto cols = ui::log_columns(page);
  REQUIRE_EQ(cols.size(), 23u);

  // In page order: CPU, BOTH GPU usage panels (dGPU then iGPU), Memory,
  // dGPU VRAM, iGPU VRAM, Temperature, Power, Network, Disk.
  const std::string want_metrics[23] = {
      "cpu.usage",           "cpu.freq_avg_mhz",   "cpu.freq_max_mhz",
      "gpu.0000:01:00.0.usage", "gpu.0000:01:00.0.core_clock_mhz",
      "gpu.0000:07:00.0.usage", "gpu.0000:07:00.0.core_clock_mhz",
      "mem.ram_used",         "mem.swap_used",
      "gpu.0000:01:00.0.vram", "gpu.0000:01:00.0.mem_clock_mhz",
      "gpu.0000:07:00.0.gtt", "gpu.0000:07:00.0.vram",
      "gpu.0000:07:00.0.mem_clock_mhz",
      "cpu.temp",             "gpu.0000:01:00.0.temp",
      "gpu.0000:07:00.0.temp",
      "cpu.power",            "gpu.0000:01:00.0.power",
      "net.total.rx",         "net.total.tx",
      "disk.total.r",         "disk.total.w"};
  for (int i = 0; i < 23; ++i) CHECK_EQ(cols[i].metric, want_metrics[i]);

  // v1.5.1: every column carries an explicit log name ("<device>
  // <quantity>"), so the names never depend on which other panels are
  // visible — there is no panel-title prefixing. The CPU usage series (no
  // legend) still folds into its "Total Usage" info row.
  const std::string want_headers[23] = {
      "CPU Usage [%]",
      "CPU Avg Frequency [MHz]",
      "CPU Max Frequency [MHz]",
      dn + " Usage [%]",
      dn + " Core Clock [MHz]",
      an + " Usage [%]",
      an + " Core Clock [MHz]",
      "RAM Used [B]",
      "Swap Used [B]",
      dn + " VRAM Used [B]",
      dn + " Memory Clock [MHz]",
      an + " GTT Used [B]",
      an + " VRAM Used [B]",
      an + " Memory Clock [MHz]",
      "CPU Temperature [°C]",
      dn + " Temperature [°C]",
      an + " Temperature [°C]",
      "CPU Package Power (APU PPT) [W]",
      dn + " Power [W]",
      "Network Download [B/s]",
      "Network Upload [B/s]",
      "Disk Read [B/s]",
      "Disk Write [B/s]"};
  for (int i = 0; i < 23; ++i) CHECK_EQ(cols[i].header, want_headers[i]);
}

TEST_CASE("columns: hiding ANY panel removes exactly its columns and never renames the rest") {
  const auto hw = demo_hw();
  const auto full_page = ui::build_default_page(hw);
  const auto full = ui::log_columns(full_page);
  REQUIRE_EQ(full.size(), 23u);

  // Every panel, one at a time, each on a FRESH page (hiding is cumulative).
  // Expected: the full header list minus exactly this panel's de-dup keys —
  // same order, same names. v1.5.1 names are explicit ("<device>
  // <quantity>"), so hiding a panel can never change a neighbour's column
  // (the v1.5.0 panel-title prefix is gone).
  for (const auto& hp : full_page) {
    auto page = ui::build_default_page(hw);
    std::vector<std::string> owned;  // this panel's logged de-dup keys
    for (auto& p : page) {
      if (p.id != hp.id) continue;
      p.visible = false;
      for (const auto& s : p.series)
        if (!s.hidden && s.numeric_fn)
          owned.push_back(s.metric.empty() ? s.id : s.metric);
      for (const auto& r : p.info_rows)
        if (r.numeric_fn && !r.metric.empty()) owned.push_back(r.metric);
    }
    const auto cols = ui::log_columns(page);
    std::vector<std::string> want;
    for (const auto& c : full)
      if (std::find(owned.begin(), owned.end(), c.metric) == owned.end())
        want.push_back(c.header);
    CHECK_EQ(cols.size(), want.size());
    for (std::size_t i = 0; i < cols.size(); ++i)
      CHECK_EQ(cols[i].header, want[i]);
  }

  // Hiding a single SERIES (the CPU temperature line): one column goes, the
  // rest of the panel stays, and the survivors keep their names.
  {
    auto page = ui::build_default_page(hw);
    for (auto& p : page)
      if (p.id == "temps") p.series[0].hidden = true;  // cpu.temp
    const auto cols = ui::log_columns(page);
    CHECK_EQ(cols.size(), 22u);
    CHECK(std::none_of(cols.begin(), cols.end(), [](const ui::LogColumn& c) {
      return c.metric == "cpu.temp";
    }));
    // Both GPU temperature columns survive, with their explicit names.
    const std::string dn = ui::gpu_short_name(hw.gpus[0]);
    const std::string an = ui::gpu_short_name(hw.gpus[1]);
    CHECK(std::any_of(cols.begin(), cols.end(), [&](const ui::LogColumn& c) {
      return c.header == dn + " Temperature [°C]";
    }));
    CHECK(std::any_of(cols.begin(), cols.end(), [&](const ui::LogColumn& c) {
      return c.header == an + " Temperature [°C]";
    }));
  }
}

TEST_CASE("columns: a GPU rename flows into the log headers") {
  auto hw = demo_hw();
  hw.gpus[0].name = "My Card";
  hw.gpus[0].renamed = true;
  const auto page = ui::build_default_page(hw);
  const auto cols = ui::log_columns(page);
  const auto has = [&](const std::string& h) {
    return std::any_of(cols.begin(), cols.end(), [&](const ui::LogColumn& c) {
      return c.header == h;
    });
  };
  // The rename touches EXACTLY the renamed GPU's columns: every GPU-derived
  // header (usage, clocks, VRAM, temp, power) follows the rename ...
  CHECK(has("My Card Usage [%]"));
  CHECK(has("My Card Core Clock [MHz]"));
  CHECK(has("My Card VRAM Used [B]"));
  CHECK(has("My Card Memory Clock [MHz]"));
  CHECK(has("My Card Temperature [°C]"));
  CHECK(has("My Card Power [W]"));
  // ... and no header left references the old detected name.
  CHECK(std::none_of(cols.begin(), cols.end(), [](const ui::LogColumn& c) {
    return c.header.find("RTX 3050") != std::string::npos;
  }));
  // The iGPU's columns are byte-for-byte what they were before the rename.
  const std::string an = ui::gpu_short_name(hw.gpus[1]);
  const std::string an_headers[6] = {
      an + " Usage [%]",         an + " Core Clock [MHz]",
      an + " GTT Used [B]",      an + " VRAM Used [B]",
      an + " Memory Clock [MHz]", an + " Temperature [°C]"};
  for (const auto& h : an_headers) CHECK(has(h));
}

TEST_CASE("columns: two GPUs with the same display name get ' #2' on the later one") {
  auto hw = demo_hw();
  hw.gpus[0].name = "Same Name";
  hw.gpus[0].renamed = true;
  hw.gpus[1].name = "Same Name";
  hw.gpus[1].renamed = true;
  const auto page = ui::build_default_page(hw);
  const auto cols = ui::log_columns(page);
  const auto has = [&](const std::string& h) {
    return std::any_of(cols.begin(), cols.end(), [&](const ui::LogColumn& c) {
      return c.header == h;
    });
  };
  // The first GPU (PCI order) keeps the base name; the later occurrence of
  // the SAME "<device> <quantity> [unit]" gets " #2".
  CHECK(has("Same Name Usage [%]"));
  CHECK(has("Same Name #2 Usage [%]"));
  CHECK(has("Same Name Core Clock [MHz]"));
  CHECK(has("Same Name #2 Core Clock [MHz]"));
  CHECK(has("Same Name VRAM Used [B]"));
  CHECK(has("Same Name #2 VRAM Used [B]"));
  CHECK(has("Same Name Memory Clock [MHz]"));
  CHECK(has("Same Name #2 Memory Clock [MHz]"));
  CHECK(has("Same Name Temperature [°C]"));
  CHECK(has("Same Name #2 Temperature [°C]"));
  // Only the dGPU reports power — no tie there.
  CHECK(has("Same Name Power [W]"));
  CHECK_FALSE(has("Same Name #2 Power [W]"));
  // The iGPU's GTT column has no twin (single GTT series).
  CHECK(has("Same Name GTT Used [B]"));
  CHECK_FALSE(has("Same Name #2 GTT Used [B]"));
  // No header is duplicated.
  for (std::size_t i = 0; i < cols.size(); ++i)
    for (std::size_t j = i + 1; j < cols.size(); ++j)
      CHECK(cols[i].header != cols[j].header);
}

TEST_CASE("values: exact precision, '.' decimal, empty for missing/NaN, monotonic elapsed") {
  const auto hw = demo_hw();
  const auto page = ui::build_default_page(hw);
  const std::filesystem::path dir = fresh_tmp_dir("values");
  ui::CsvLogger log;
  std::string err;
  const std::string p = log.start(page, dir.string(), err);
  REQUIRE(!p.empty());

  const TimePoint t0 = Clock::now();
  REQUIRE(log.tick(page, mk_snap(t0), false, err));
  REQUIRE(log.tick(page, mk_snap(t0 + std::chrono::milliseconds(1250)), false, err));
  REQUIRE(log.tick(page, mk_snap(t0 + std::chrono::milliseconds(2500)), false, err));
  log.stop();

  const auto lines = lines_of_file(p);
  REQUIRE_EQ(lines.size(), 4u);  // header + 3 rows
  const auto row0 = split_csv_line(lines[1]);
  REQUIRE_EQ(row0.size(), 25u);  // timestamp + elapsed_s + 23 values

  // --- timestamp: local ISO 8601 with ms and UTC offset --------------------
  const std::string ts = row0[0];
  // 2026-10-22T14:30:05.123+05:30 is 29 chars: the sign is at 23, the offset
  // hour at 24-25, the ':' at 26, the offset minute at 27-28.
  CHECK_EQ(ts.size(), 29u);
  CHECK(ts[4] == '-');
  CHECK(ts[7] == '-');
  CHECK(ts[10] == 'T');
  CHECK(ts[13] == ':');
  CHECK(ts[16] == ':');
  CHECK(ts[19] == '.');
  CHECK(ts[26] == ':');
  const bool is_sign = (ts[23] == '+') || (ts[23] == '-');
  CHECK(is_sign);
  for (const int i : {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15, 17, 18, 20, 21,
                      22, 24, 25, 27, 28})
    CHECK(std::isdigit(static_cast<unsigned char>(ts[i])));
  struct tm tmv{};
  CHECK(strptime(ts.substr(0, 19).c_str(), "%Y-%m-%dT%H:%M:%S", &tmv) !=
        nullptr);
  // The written offset matches the machine's actual UTC offset.
  const long long off_min = machine_offset_min();
  const int exp_h = static_cast<int>(std::llabs(off_min) / 60);
  const int exp_m = static_cast<int>(std::llabs(off_min) % 60);
  const int got_h = std::atoi(ts.substr(24, 2).c_str());
  const int got_m = std::atoi(ts.substr(27, 2).c_str());
  const int got_h_signed = (ts[23] == '-') ? -got_h : got_h;
  CHECK_EQ(got_h_signed, exp_h);
  CHECK_EQ(got_m, exp_m);

  // --- elapsed_s: since THIS file started, 3 decimals, monotonic -----------
  CHECK_EQ(row0[1], std::string("0.000"));
  CHECK_EQ(split_csv_line(lines[2])[1], std::string("1.250"));
  CHECK_EQ(split_csv_line(lines[3])[1], std::string("2.500"));

  // --- values: the precision rules, and empty (never 0) for missing/NaN ----
  // Same order as the default-page columns above (both GPU usage panels
  // before Memory; the iGPU core clock is NaN → empty).
  const std::string want[23] = {
      "42.6",        "2400",        "2800",
      "77.6",        "1650",
      "12.3",        "",
      "10737418240", "",
      "5905580646",  "1750",
      "322122547",   "429496730",   "1600",
      "55.0",        "47.0",        "42.0",
      "12.35",       "28.00",
      "1048576",     "524289",
      "4194304",     "1048578"};
  for (int i = 0; i < 23; ++i) CHECK_EQ(row0[2 + i], want[i]);
}

TEST_CASE("values: a de_DE locale changes nothing (manual formatting, never %f)") {
  const auto hw = demo_hw();
  const auto page = ui::build_default_page(hw);
  const std::filesystem::path dir = fresh_tmp_dir("locale");

  ui::CsvLogger logA;
  std::string err;
  const std::string pA = logA.start(page, dir.string(), err);
  REQUIRE(!pA.empty());
  REQUIRE(logA.tick(page, mk_snap(Clock::now()), false, err));
  logA.stop();

  // Switch to a locale that uses ',' as the decimal point, log one more row,
  // switch back. (If the locale is absent the comparison still holds — the
  // formatter is locale-proof by construction.)
  const std::string saved = std::setlocale(LC_ALL, nullptr);
  const bool de = std::setlocale(LC_ALL, "de_DE.UTF-8") != nullptr;
  CHECK(de);
  ui::CsvLogger logB;
  const std::string pB = logB.start(page, dir.string(), err);
  REQUIRE(!pB.empty());
  REQUIRE(logB.tick(page, mk_snap(Clock::now()), false, err));
  logB.stop();
  std::setlocale(LC_ALL, saved.c_str());

  const auto a = lines_of_file(pA);
  const auto b = lines_of_file(pB);
  REQUIRE_EQ(a.size(), 2u);
  REQUIRE_EQ(b.size(), 2u);
  CHECK_EQ(a[0], b[0]);  // identical header
  const auto fa = split_csv_line(a[1]);
  const auto fb = split_csv_line(b[1]);
  REQUIRE_EQ(fa.size(), fb.size());
  for (std::size_t i = 2; i < fa.size(); ++i) CHECK_EQ(fa[i], fb[i]);
}

TEST_CASE("quoting: a GPU renamed to a name with quotes and commas") {
  auto hw = demo_hw();
  hw.gpus[0].name = "Card \"A\", fast";
  hw.gpus[0].renamed = true;
  const auto page = ui::build_default_page(hw);
  const std::filesystem::path dir = fresh_tmp_dir("quoting");
  ui::CsvLogger log;
  std::string err;
  const std::string p = log.start(page, dir.string(), err);
  REQUIRE(!p.empty());

  const auto lines = lines_of_file(p);
  REQUIRE_EQ(lines.size(), 1u);
  // The raw file quotes the field and doubles the quotes ...
  CHECK(lines[0].find("\"Card \"\"A\"\", fast Usage [%]\"") !=
        std::string::npos);
  // ... and a CSV parser reads the field back exactly.
  const auto hdr = split_csv_line(lines[0]);
  CHECK(std::find(hdr.begin(), hdr.end(), std::string("Card \"A\", fast Usage [%]"))
        != hdr.end());
  // The rename reaches every GPU-derived header (VRAM and Memory Clock
  // included), each quoted when it must be.
  CHECK(std::find(hdr.begin(), hdr.end(),
                  std::string("Card \"A\", fast VRAM Used [B]"))
        != hdr.end());
  CHECK(std::find(hdr.begin(), hdr.end(),
                  std::string("Card \"A\", fast Memory Clock [MHz]"))
        != hdr.end());
  CHECK(std::find(hdr.begin(), hdr.end(),
                  std::string("Card \"A\", fast Temperature [°C]"))
        != hdr.end());
}

TEST_CASE("rotation: a page change starts the next file; a same-page tick does not") {
  const auto hw = demo_hw();
  auto page = ui::build_default_page(hw);
  const std::string an = ui::gpu_short_name(hw.gpus[1]);
  const std::filesystem::path dir = fresh_tmp_dir("rotation");

  ui::CsvLogger log;
  std::string err;
  const std::string p1 = log.start(page, dir.string(), err);
  REQUIRE(!p1.empty());
  const TimePoint t0 = Clock::now();
  REQUIRE(log.tick(page, mk_snap(t0), false, err));

  // The mutation flag set but the page UNCHANGED: no rotation.
  REQUIRE(log.tick(page, mk_snap(t0 + std::chrono::seconds(1)), true, err));
  REQUIRE_EQ(csv_files_in(dir.string()).size(), 1u);
  REQUIRE_EQ(log.row_count(), 2u);

  // Hide the iGPU usage panel: the next tick starts base-2.csv.
  for (auto& p : page)
    if (p.id == "gpu.0000:07:00.0.usage") p.visible = false;
  REQUIRE(log.tick(page, mk_snap(t0 + std::chrono::seconds(2)), true, err));

  const std::string p2 = std::filesystem::path(p1).parent_path().string() +
                         "/" + std::filesystem::path(p1).stem().string() +
                         "-2.csv";
  CHECK(std::filesystem::exists(p2));
  CHECK_EQ(log.path(), p2);
  CHECK_EQ(log.row_count(), 1u);  // rows restart with the new file
  REQUIRE_EQ(csv_files_in(dir.string()).size(), 2u);

  const auto l1 = lines_of_file(p1);
  const auto l2 = lines_of_file(p2);
  REQUIRE_EQ(l1.size(), 3u);  // header + 2 rows
  REQUIRE_EQ(l2.size(), 2u);  // header + 1 row
  // The hidden panel's columns are gone from file 2's header; the dGPU's own
  // core clock column keeps its explicit name (nothing to re-qualify).
  const std::string dn = ui::gpu_short_name(hw.gpus[0]);
  CHECK(l2[0].find(an + " Usage [%]") == std::string::npos);
  CHECK(l2[0].find(an + " Core Clock [MHz]") == std::string::npos);
  const auto hdr2 = split_csv_line(l2[0]);
  CHECK(std::find(hdr2.begin(), hdr2.end(), dn + " Core Clock [MHz]")
        != hdr2.end());
  // File 1 keeps its header (the iGPU's columns included) and its rows.
  CHECK(l1[0].find(an + " Core Clock [MHz]") != std::string::npos);
  const auto hdr1 = split_csv_line(l1[0]);
  CHECK_EQ(hdr1.size() - hdr2.size(), 2u);
}

TEST_CASE("sessions: start/stop/start writes two files, rows match the ticks") {
  const auto page = ui::build_default_page(demo_hw());
  const std::filesystem::path dir = fresh_tmp_dir("sessions");
  ui::CsvLogger log;
  std::string err;
  const std::string p1 = log.start(page, dir.string(), err);
  REQUIRE(!p1.empty());
  const TimePoint t0 = Clock::now();
  for (int i = 0; i < 3; ++i)
    REQUIRE(log.tick(page, mk_snap(t0 + std::chrono::milliseconds(100 * i)), false,
                     err));
  CHECK_EQ(log.row_count(), 3u);
  log.stop();
  CHECK_FALSE(log.active());

  // A new session gets a fresh file — never overwriting file 1, even when
  // both starts land in the same second (the O_EXCL open bumps the suffix).
  const std::string p2 = log.start(page, dir.string(), err);
  CHECK(!p2.empty());
  CHECK(p2 != p1);
  REQUIRE(log.tick(page, mk_snap(Clock::now()), false, err));
  REQUIRE(log.tick(page, mk_snap(Clock::now()), false, err));
  log.stop();
  CHECK_EQ(log.row_count(), 2u);

  REQUIRE_EQ(csv_files_in(dir.string()).size(), 2u);
  CHECK_EQ(lines_of_file(p1).size(), 4u);  // header + 3 rows
  CHECK_EQ(lines_of_file(p2).size(), 3u);  // header + 2 rows
}

TEST_CASE("errors: an uncreatable directory stops logging cleanly") {
  const std::filesystem::path base = fresh_tmp_dir("errors");
  const std::filesystem::path blocker = base / "blocker";
  {  // a REGULAR FILE where the log dir's parent must be a directory
    std::ofstream f(blocker, std::ios::binary | std::ios::trunc);
    f << "x";
  }
  const auto page = ui::build_default_page(demo_hw());
  ui::CsvLogger log;
  std::string err;
  const std::string p = log.start(page, (blocker / "sub" / "logs").string(), err);
  CHECK(p.empty());
  CHECK_FALSE(err.empty());
  CHECK_FALSE(log.active());
  // Ticks with no active logger are a clean no-op, not a crash.
  const Snapshot snap = mk_snap(Clock::now());
  CHECK_FALSE(log.tick(page, snap, false, err));
}

TEST_CASE("guard: GETH_TEST=1 refuses the real ~/.local/share, allows a temp XDG_DATA_HOME") {
  const auto page = ui::build_default_page(demo_hw());
  REQUIRE(std::getenv("HOME") != nullptr);

  // 1) GETH_TEST=1, no XDG_DATA_HOME: the default dir IS under the real
  // ~/.local/share -> refused.
  {
    ScopedEnv xdg("XDG_DATA_HOME", "");  // unset
    ScopedEnv test("GETH_TEST", "1");
    ui::CsvLogger log;
    std::string err;
    const std::string p = log.start(page, /*dir=*/"", err);
    CHECK(p.empty());
    CHECK(err.find("GETH_TEST") != std::string::npos);
    CHECK_FALSE(log.active());
  }
  // 2) GETH_TEST=1 with a temp XDG_DATA_HOME: allowed.
  {
    const std::filesystem::path xdg = fresh_tmp_dir("guard_ok");
    ScopedEnv xdg_env("XDG_DATA_HOME", xdg.string());
    ScopedEnv test("GETH_TEST", "1");
    ui::CsvLogger log;
    std::string err;
    const std::string p = log.start(page, /*dir=*/"", err);
    CHECK(!p.empty());
    CHECK(std::filesystem::exists(p));
    CHECK(p.find(xdg.string()) == 0);  // the file landed under the temp dir
  }
}

TEST_CASE("rec_label: '● REC hh:mm:ss ' while active, empty after stop") {
  const auto page = ui::build_default_page(demo_hw());
  const std::filesystem::path dir = fresh_tmp_dir("rec");
  ui::CsvLogger log;
  std::string err;
  REQUIRE(!log.start(page, dir.string(), err).empty());
  const std::string lbl = log.rec_label();
  CHECK(lbl.rfind("\xE2\x97\x8F REC ", 0) == 0);  // "● REC "
  CHECK(lbl.back() == ' ');
  CHECK_EQ(lbl.size(), 17u);  // "● REC " (7 bytes) + hh:mm:ss (8) + " " (1)
  const std::string t = lbl.substr(8, 8);  // hh:mm:ss
  CHECK(t[2] == ':');
  CHECK(t[5] == ':');
  for (const int i : {0, 1, 3, 4, 6, 7})
    CHECK(std::isdigit(static_cast<unsigned char>(t[i])));
  log.stop();
  CHECK(log.rec_label().empty());
}

TEST_CASE("allocs: zero heap allocations per logged row after warm-up") {
  const auto page = ui::build_default_page(demo_hw());
  const std::filesystem::path dir = fresh_tmp_dir("allocs");
  ui::CsvLogger log;
  std::string err;
  REQUIRE(!log.start(page, dir.string(), err).empty());
  const TimePoint t0 = Clock::now();
  const Snapshot snap = mk_snap(t0);  // ONE snapshot, reused for every tick
  for (int i = 0; i < 5; ++i) REQUIRE(log.tick(page, snap, false, err));
  geth::set_alloc_counting(true);
  const long long a0 = geth::alloc_count();
  bool ok = true;
  for (int i = 0; i < 100; ++i) ok = ok && log.tick(page, snap, false, err);
  const long long delta = geth::alloc_count() - a0;
  geth::set_alloc_counting(false);
  CHECK(ok);
  CHECK_EQ(delta, 0);
}

}  // TEST_SUITE
