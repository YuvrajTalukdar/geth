// Disk read/write rates from /proc/diskstats (sector counters, x512 = bytes).
//
// Only whole disks are reported: partitions ("sda1" of "sda"), virtual
// devices (loop*, ram*, zram*), device-mapper (dm-*), CD-ROMs (sr*) and RAID
// (md*) are noise for a system monitor and would double-count the data.
#pragma once

#include "core/source.hpp"
#include "util/sysfs_file.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace geth {

struct DiskRow {
  std::string name;
  unsigned long long sectors_read = 0;
  unsigned long long sectors_written = 0;
};

// Pure over string_view for testing. Field map (kernel >= 4.18 layout; the
// first 10 fields have been stable since 2.4):
//   0 maj  1 min  2 name  3 reads  4 merges_r  5 SECTORS_READ
//   6 ms_r  7 writes  8 merges_w  9 SECTORS_WRITTEN ...
std::vector<DiskRow> parse_diskstats(std::string_view text);

// True if `name` is a partition of another listed device.
bool is_partition(const std::string& name, const std::vector<std::string>& all_names);

// Virtual/RAID/CD-ROM prefixes that are never whole disks we want to show.
bool skip_disk_device(const std::string& name);

class DiskSource : public Source {
 public:
  const char* name() const override { return "Disk"; }
  SourceStatus probe() override;
  void sample(Snapshot& out) override;

  // Test seam (mirrors CpuSource::set_test_stat): make probe() and sample()
  // parse THIS /proc/diskstats text instead of reading the real file. A
  // removal test "unplugs" a disk mid-run by setting new text without it —
  // something that can't happen for real in a test. Call before probe_all()
  // so the warm-up baseline uses it too; it stays on for the source's life.
  void set_test_diskstats(std::string text) {
    _test_diskstats = std::move(text);
    _test_diskstats_on = true;
  }

 private:
  struct DiskDev {
    std::string name;
    unsigned long long prev_r = 0;
    unsigned long long prev_w = 0;
    bool have_prev = false;
  };

  // The /proc/diskstats text for this tick: the test seam when set, else the
  // live file. probe() fixes the whole-disk set from it and sample() computes
  // rates — so a seam change removes a disk mid-run. The live read aliases
  // SysfsFile's reused buffer; the seam is a stable member string.
  const std::string& diskstats_text() {
    if (_test_diskstats_on) return _test_diskstats;
    return _diskstats.read();
  }

  SysfsFile _diskstats;         // /proc/diskstats
  std::vector<DiskDev> _devs;   // whole disks, fixed at probe
  TimePoint _prev_tp;
  bool _have_prev = false;
  bool _sized = false;  // snapshot's device slots sized once
  std::string _test_diskstats;  // test seam (see set_test_diskstats)
  bool _test_diskstats_on = false;
};

}  // namespace geth
