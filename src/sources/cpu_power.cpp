#include "sources/cpu_power.hpp"

#include "sources/sensor_tables.hpp"
#include "util/parse.hpp"

#include <cerrno>
#include <filesystem>
#include <fstream>

namespace geth {

std::optional<unsigned long long> counter_delta(unsigned long long now, unsigned long long prev,
                                                unsigned long long max_range) {
  // A RAPL energy counter is monotonic and wraps at max_range
  // (max_energy_range_uj). Between two samples it either advances without
  // wrapping (now >= prev) or wraps exactly once (now < prev). An advance of
  // more than one full scale in one tick means a reset/fault, not multiple
  // wraps, so we refuse it.
  if (now >= prev) {
    const unsigned long long d = now - prev;
    if (max_range == 0 || d <= max_range) return d;
    return std::nullopt;
  }
  // now < prev: the counter wrapped once. Without the scale the wrap
  // distance is unknowable (the counter could have been reset instead).
  if (max_range == 0) return std::nullopt;
  return max_range - (prev - now);
}

namespace {

// Reads the (short) `name` attribute of a powercap/hwmon device.
std::string read_attr(const std::filesystem::path& dir) {
  std::ifstream f(dir / "name");
  std::string v;
  std::getline(f, v);
  return v;
}

// True if /proc/cpuinfo names an AMD vendor. Used only to phrase the
// probe's APU-PPT note; the Collector performs the actual lift.
bool cpu_vendor_is_amd() {
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (std::getline(f, line))
    if (line.rfind("vendor_id", 0) == 0)
      return line.find("AuthenticAMD") != std::string::npos;
  return false;
}

}  // namespace

SourceStatus CpuPowerSource::probe() {
  SourceStatus st;
  st.description = name();

  // The preference order is DATA (sensor_tables.hpp):
  //   RAPL > amd_energy > zenpower > APU PPT (injected by the Collector).
  const auto chain_line = [] {
    return std::string("\n  chain: RAPL > amd_energy > zenpower; if none, the amdgpu PPT") +
           " of an integrated GPU (see GPU section)";
  };

  // --- RAPL powercap --------------------------------------------------------
  // Enumerate every zone under /sys/class/powercap and select the devices
  // whose `name` starts with "package" (one per socket). Both the zone itself
  // and its child domains are candidates: layout differs between kernels —
  // on this AMD machine the zone *is* the package domain (intel-rapl:0,
  // name=package-0) while the child (name=core) must be skipped.
  int eacces = 0;
  const std::string root = "/sys/class/powercap";
  if (std::filesystem::is_directory(root)) {
    auto zones = std::filesystem::directory_iterator(root);
    for (auto it = zones; it != std::filesystem::directory_iterator(); ++it) {
      if (!it->is_directory()) continue;
      auto consider = [&](const std::filesystem::path& dev) {
        if (!read_attr(dev).starts_with("package")) return;
        Domain d;
        int err = 0;
        const std::string epath = (dev / "energy_uj").string();
        if (!SysfsFile::open(epath, d.energy, err)) {
          if (err == EACCES) ++eacces;
          return;
        }
        // Full counter scale; optional — some drivers do not publish it.
        SysfsFile::open((dev / "max_energy_range_uj").string(), d.range, err);
        if (st.paths.size() < 4) st.paths.push_back(d.energy.path());
        _domains.push_back(std::move(d));
      };
      consider(it->path());
      auto children = std::filesystem::directory_iterator(it->path());
      for (auto c = children; c != std::filesystem::directory_iterator(); ++c) {
        if (c->is_directory()) consider(c->path());
      }
    }
  }
  if (!_domains.empty()) {
    _backend = CpuPackagePowerSource::kRapl;
    st.available = true;
    st.detail = "RAPL package domains: " + std::to_string(_domains.size()) + chain_line();
    return st;
  }
  if (eacces > 0) {
    // The EACCES path is where this machine lands: RAPL exists but is
    // root-only, so the source ACTUALLY supplying CPU package power at sample
    // time is the integrated GPU's PPT (Collector lifts it). --probe must say
    // so.
    st.detail = "RAPL energy_uj is readable only by root on kernel >= 5.10 (EACCES)" +
                chain_line();
    if (cpu_vendor_is_amd())
      st.detail +=
          "\n  supplying now: amdgpu PPT of the integrated GPU — APU package"
          " power includes iGPU (see GPU section)";
    st.hint =
        "install packaging/99-geth-rapl.rules into /etc/udev/rules.d/ and run "
        "'sudo udevadm trigger --subsystem-match=powercap' — or run geth as root";
    return st;
  }

  // --- AMD hwmon energy counters, in table order -----------------------------
  for (auto source : cpu_power_counter_sources()) {
    const char* driver = nullptr;
    if (source == CpuPackagePowerSource::kAmdEnergy)
      driver = "amd_energy";
    else if (source == CpuPackagePowerSource::kZenpower)
      driver = "zenpower";
    if (!driver) continue;

    const std::string hwmon = "/sys/class/hwmon";
    if (!std::filesystem::is_directory(hwmon)) break;
    auto it = std::filesystem::directory_iterator(hwmon);
    for (; it != std::filesystem::directory_iterator(); ++it) {
      if (!it->is_directory() || read_attr(it->path()) != driver) continue;
      Domain d;
      int err = 0;
      const std::string epath = (it->path() / "power1_raw").string();
      if (!SysfsFile::open(epath, d.energy, err)) return st;  // driver present but broken
      // Try the scale under either known name; harmless if absent.
      SysfsFile::open((it->path() / "max_energy_range_uj").string(), d.range, err);
      st.paths.push_back(d.energy.path());
      _backend = source;
      st.available = true;
      st.detail = std::string(driver) + " hwmon power1_raw (cumulative microjoules)" +
                  chain_line();
      _domains.push_back(std::move(d));
      return st;
    }
  }

  st.detail = "no RAPL package domain and no amd_energy/zenpower hwmon driver found";
  if (cpu_vendor_is_amd())
    st.detail +=
        "\n  if an integrated AMD GPU exists, its PPT supplies CPU package"
        " power (APU package power includes iGPU — see GPU section)";
  st.hint = "on AMD: 'sudo modprobe zenpower' may expose a package energy counter";
  return st;
}

void CpuPowerSource::sample(Snapshot& out) {
  if (_domains.empty()) return;
  const TimePoint now = Clock::now();

  std::vector<unsigned long long> vals(_domains.size());
  bool read_ok = true;
  for (std::size_t i = 0; i < _domains.size(); ++i) {
    const auto v = parse::to_u64(parse::trim(_domains[i].energy.read()));
    if (!v) {
      read_ok = false;
      break;
    }
    vals[i] = *v;
  }
  if (!read_ok) return;

  // First sample: nothing to diff against, so just remember the baseline.
  if (!_have_prev_tp) {
    for (std::size_t i = 0; i < _domains.size(); ++i) _domains[i].prev_uj = vals[i];
    _have_prev_tp = true;
    _prev_tp = now;
    return;
  }

  double total_uj = 0.0;
  bool ok = true;
  for (std::size_t i = 0; i < _domains.size(); ++i) {
    unsigned long long range = 0;
    if (_domains[i].range.valid()) {
      const auto r = parse::to_u64(parse::trim(_domains[i].range.read()));
      if (r) range = *r;
    }
    const auto delta = counter_delta(vals[i], _domains[i].prev_uj, range);
    if (!delta) {
      ok = false;  // refuse to report a partial package total
      break;
    }
    total_uj += static_cast<double>(*delta);
    _domains[i].prev_uj = vals[i];
  }
  if (ok) {
    // Real elapsed time between samples, not the nominal interval: the tick
    // period drifts, and the machine may have just woken from suspend.
    const double dt = std::chrono::duration<double>(now - _prev_tp).count();
    if (dt > 0.0 && dt < 604800.0) {  // >1 week elapsed: stale baseline, skip
      out.cpu.package_power_w = total_uj / 1e6 / dt;  // microjoules -> joules -> watts
      out.cpu.package_power_source = _backend;
    }
  }
  _prev_tp = now;
}

}  // namespace geth
