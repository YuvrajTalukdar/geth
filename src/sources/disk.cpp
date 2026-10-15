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

namespace {

// Tokenizes the first 10 fields of one /proc/diskstats line and returns the
// device name (field 2) plus sectors_read (5) and sectors_written (9).
// Hand-tokenized: the per-tick hot path must not build token vectors.
bool diskstat_row(std::string_view line, std::string_view& name,
                  unsigned long long& sr, unsigned long long& sw) {
  std::string_view tok[10];
  int n = 0;
  while (n < 10) {
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
      line = line.substr(1);
    std::size_t i = 0;
    while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
    if (i == 0) break;  // ran out of input
    tok[n] = line.substr(0, i);
    ++n;
    if (i >= line.size()) break;
    line = line.substr(i);
  }
  if (n != 10) return false;
  name = tok[2];
  const auto r = parse::to_u64(tok[5]);
  const auto w = parse::to_u64(tok[9]);
  if (!r || !w) return false;
  sr = *r;
  sw = *w;
  return true;
}

}  // namespace

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

  // Fix the whole-disk set (cold path): partitions, loop/ram/zram/dm-*/sr*/
  // md* are noise and would double-count the data.
  const auto rows = parse_diskstats(_diskstats.read());
  std::vector<std::string> names;
  names.reserve(rows.size());
  for (const auto& r : rows) names.push_back(r.name);
  _devs.clear();
  for (const auto& r : rows) {
    if (skip_disk_device(r.name) || is_partition(r.name, names)) continue;
    DiskDev d;
    d.name = r.name;
    _devs.push_back(std::move(d));
  }
  st.available = true;
  return st;
}

void DiskSource::sample(Snapshot& out) {
  const TimePoint now = Clock::now();
  const double dt = std::chrono::duration<double>(now - _prev_tp).count();
  const bool usable = _have_prev && dt > 0.0 && dt < 604800.0;

  if (!_sized) {
    // One-time: size the reused Snapshot's device slots + names.
    out.disk.devices.assign(_devs.size(), DiskDeviceSample{});
    for (std::size_t i = 0; i < _devs.size(); ++i)
      out.disk.devices[i].name = _devs[i].name;
    _sized = true;
  }

  // Devices missing from this tick's /proc/diskstats get a gap (nullopt),
  // never a stale or fake 0.
  for (auto& d : out.disk.devices) {
    d.read_bytes_per_s.reset();
    d.write_bytes_per_s.reset();
  }
  out.disk.total_read_bytes_per_s.reset();
  out.disk.total_write_bytes_per_s.reset();

  double tot_r = 0.0, tot_w = 0.0;
  bool any_r = false, any_w = false;
  const std::string& text = _diskstats.read();
  parse::for_each_line(text, [&](std::string_view line) {
    std::string_view name;
    unsigned long long sr = 0, sw = 0;
    if (!diskstat_row(line, name, sr, sw)) return true;
    int idx = -1;
    for (std::size_t i = 0; i < _devs.size(); ++i)
      if (name == std::string_view(_devs[i].name)) {
        idx = static_cast<int>(i);
        break;
      }
    if (idx < 0) return true;  // partition / virtual / not probed: ignore
    DiskDev& dv = _devs[static_cast<std::size_t>(idx)];
    if (usable && dv.have_prev) {
      // Unsigned: sector counters are monotonic; a delta of 0 means idle.
      const unsigned long long dr = sr - dv.prev_r;
      const unsigned long long dw = sw - dv.prev_w;
      out.disk.devices[static_cast<std::size_t>(idx)].read_bytes_per_s =
          static_cast<double>(dr) * 512.0 / dt;
      out.disk.devices[static_cast<std::size_t>(idx)].write_bytes_per_s =
          static_cast<double>(dw) * 512.0 / dt;
      tot_r += static_cast<double>(dr) * 512.0 / dt;
      tot_w += static_cast<double>(dw) * 512.0 / dt;
      any_r = any_w = true;
    }
    dv.prev_r = sr;
    dv.prev_w = sw;
    dv.have_prev = true;
    return true;
  });
  if (any_r) out.disk.total_read_bytes_per_s = tot_r;
  if (any_w) out.disk.total_write_bytes_per_s = tot_w;

  _prev_tp = now;
  _have_prev = true;
}

}  // namespace geth
