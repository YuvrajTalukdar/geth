#include "sources/disk.hpp"

#include "util/parse.hpp"

#include <algorithm>

namespace geth {

std::vector<DiskRow> parse_diskstats(std::string_view text) {
  std::vector<DiskRow> out;
  for (const std::string_view line : parse::split_lines(text)) {
    const auto toks = parse::tokens(line);
    if (toks.size() < 10) continue;
    const auto rd = parse::to_u64(toks[5]);
    const auto wr = parse::to_u64(toks[9]);
    if (!rd || !wr) continue;
    DiskRow r;
    r.name.assign(toks[2]);
    r.sectors_read = *rd;
    r.sectors_written = *wr;
    out.push_back(std::move(r));
  }
  return out;
}

bool is_partition(const std::string& name, const std::vector<std::string>& all_names) {
  // A partition name always extends its parent device's name: SCSI appends
  // digits ("sda" -> "sda1"), NVMe appends "p" + digits ("nvme0n1" ->
  // "nvme0n1p1"). So a row is a partition iff some listed device is a strict
  // prefix of it and the next character is a digit or 'p'. (Stripping digits
  // would give "nvme0n1p" as the parent, which is not a device.)
  for (const auto& d : all_names) {
    if (d.size() >= name.size()) continue;
    if (name.compare(0, d.size(), d) != 0) continue;
    const char c = name[d.size()];
    if (c == 'p' || (c >= '0' && c <= '9')) return true;
  }
  return false;
}

bool skip_disk_device(const std::string& n) {
  auto starts = [&](const char* p) { return n.rfind(p, 0) == 0; };
  if (starts("loop") || starts("ram") || starts("zram") || starts("dm-")) return true;
  if (n.size() >= 2 && n[0] == 's' && n[1] == 'r') return true;  // sr0: CD-ROM
  if (n.size() >= 2 && n[0] == 'm' && n[1] == 'd') return true;  // md0: RAID
  return false;
}

SourceStatus DiskSource::probe() {
  SourceStatus st;
  st.description = name();
  int err = 0;
  if (!SysfsFile::open("/proc/diskstats", _diskstats, err)) {
    st.detail = "cannot open /proc/diskstats (errno " + std::to_string(err) + ")";
    return st;
  }
  st.paths.push_back("/proc/diskstats");
  st.detail = "per-device read/write rates from sector counters (x512)";
  st.available = true;
  return st;
}

void DiskSource::sample(Snapshot& out) {
  const auto rows = parse_diskstats(_diskstats.read());

  std::vector<std::string> names;
  names.reserve(rows.size());
  for (const auto& r : rows) names.push_back(r.name);

  // Keep whole disks only, in file order (stable: sorted by major/minor).
  std::vector<const DiskRow*> keep;
  for (const auto& r : rows) {
    if (skip_disk_device(r.name) || is_partition(r.name, names)) continue;
    keep.push_back(&r);
  }

  if (!_have_prev) {
    _prev = rows;
    _have_prev = true;
    _prev_tp = Clock::now();
    return;  // first sample: nothing to diff against
  }

  const double dt = std::chrono::duration<double>(Clock::now() - _prev_tp).count();
  const bool usable = dt > 0.0 && dt < 604800.0;

  double tot_r = 0.0, tot_w = 0.0;
  bool any_r = false, any_w = false;
  for (const auto* r : keep) {
    DiskDeviceSample d;
    d.name = r->name;
    if (usable) {
      const auto prev = std::find_if(_prev.begin(), _prev.end(),
                                     [&](const DiskRow& p) { return p.name == r->name; });
      if (prev != _prev.end()) {
        // Unsigned: sector counters are monotonic; a delta of 0 means idle.
        const unsigned long long dr = r->sectors_read - prev->sectors_read;
        const unsigned long long dw = r->sectors_written - prev->sectors_written;
        d.read_bytes_per_s = static_cast<double>(dr) * 512.0 / dt;
        d.write_bytes_per_s = static_cast<double>(dw) * 512.0 / dt;
        tot_r += *d.read_bytes_per_s;
        tot_w += *d.write_bytes_per_s;
        any_r = any_w = true;
      }
      // A newly appeared device (e.g. a USB stick) has no baseline yet:
      // report nothing for this tick rather than a fake 0.
    }
    out.disk.devices.push_back(std::move(d));
  }
  if (any_r) out.disk.total_read_bytes_per_s = tot_r;
  if (any_w) out.disk.total_write_bytes_per_s = tot_w;

  _prev = rows;
  _prev_tp = Clock::now();
}

}  // namespace geth
