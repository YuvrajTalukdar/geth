#include "sources/cpu_power.hpp"

#include "sources/sensor_tables.hpp"
#include "util/parse.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <filesystem>
#include <fstream>

namespace geth {

std::optional<unsigned long long> counter_delta(unsigned long long now, unsigned long long prev,
                                                unsigned long long max_range) {
  // Two kinds of counter:
  //  - RAPL: wraps at max_energy_range_uj. Between two samples it either
  //    advances without wrapping (now >= prev) or wraps exactly once
  //    (now < prev). An advance of more than one full scale in one tick is a
  //    reset/fault, not multiple wraps, so it is refused.
  //  - amd_energy: 64-bit accumulator with NO published range (max_range ==
  //    0). A backwards step can only be a driver reset, so it is refused —
  //    the caller re-anchors the baseline and skips the sample.
  if (now >= prev) {
    const unsigned long long d = now - prev;
    if (max_range == 0 || d <= max_range) return d;
    return std::nullopt;
  }
  if (max_range == 0) return std::nullopt;  // reset: no scale to wrap at
  return max_range - (prev - now);
}

namespace {

// The shared footnote for the --probe output: which source feeds CPU package
// power on this machine.
const std::string& chain_line() {
  static const std::string line =
      "\n  chain: RAPL > amd_energy > zenpower; if none, the amdgpu PPT of an"
      " integrated GPU (see GPU section)";
  return line;
}

// The first line of a file, trimmed ("") when absent.
std::string read_first_line(const std::filesystem::path& p) {
  std::ifstream f(p);
  std::string v;
  std::getline(f, v);
  const auto t = parse::trim(v);
  return std::string(t);
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

// The binary's effective capability set, straight from /proc/self/status
// (no getcap spawn). "0000000000000000" when unreadable.
std::string cap_eff_hex() {
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("CapEff:", 0) != 0) continue;
    const auto toks = parse::tokens(line);
    if (toks.size() < 2) return "unreadable";
    std::string hex;
    for (char c : toks[1])
      if (std::isxdigit(static_cast<unsigned char>(c))) hex += c;
    return hex.empty() ? "unreadable" : hex;
  }
  return "unreadable";
}

// "energy3_input" -> 3 (false when the name is not energyN_input).
bool parse_energy_index(const std::string& filename, unsigned& n) {
  if (filename.size() < 13 || filename.substr(0, 6) != "energy" ||
      filename.compare(filename.size() - 6, 6, "_input") != 0)
    return false;
  const auto d = parse::to_u64(filename.substr(6, filename.size() - 12));
  if (!d) return false;
  n = *d;
  return true;
}

}  // namespace

SourceStatus CpuPowerSource::probe() {
  SourceStatus st;
  st.description = name();

  // --- RAPL powercap --------------------------------------------------------
  // Enumerate every zone under class/powercap and select the devices whose
  // `name` starts with "package" (one per socket). Both the zone itself and
  // its child domains are candidates: layout differs between kernels — on an
  // AMD Zen machine the zone *is* the package domain (intel-rapl:0,
  // name=package-0) while the child (name=core) must be skipped.
  int eacces = 0;
  const std::string powercap = _root + "/class/powercap";
  std::error_code ec;
  if (std::filesystem::is_directory(powercap, ec)) {
    auto zones = std::filesystem::directory_iterator(powercap, ec);
    for (auto it = zones; it != std::filesystem::directory_iterator(); it.increment(ec)) {
      if (!it->is_directory(ec)) continue;
      auto consider = [&](const std::filesystem::path& dev) {
        if (read_first_line(dev / "name").rfind("package", 0) != 0) return;
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
      auto children = std::filesystem::directory_iterator(it->path(), ec);
      for (auto c = children; c != std::filesystem::directory_iterator(); c.increment(ec))
        if (c->is_directory(ec)) consider(c->path());
    }
  }
  if (!_domains.empty()) {
    _backend = CpuPackagePowerSource::kRapl;
    st.available = true;
    st.detail = "RAPL package domains: " + std::to_string(_domains.size()) + chain_line();
    return st;
  }
  if (eacces > 0) {
    // The EACCES path is where a default install lands: RAPL exists but is
    // root-only, so the source ACTUALLY supplying CPU package power at sample
    // time is the integrated GPU's PPT (Collector lifts it). --probe says so,
    // and reports the binary's capabilities so a failed setcap is visible.
    st.detail = "RAPL energy_uj is readable only by root on kernel >= 5.10 (EACCES)" +
                chain_line();
    st.detail += "\n  CapEff: 0x" + cap_eff_hex() +
                 "  (cap_dac_read_search is bit 2)";
    if (cpu_vendor_is_amd())
      st.detail +=
          "\n  supplying now: amdgpu PPT of the integrated GPU — APU package"
          " power includes iGPU (see GPU section)";
    st.hint =
        "run 'sudo cmake --build . --target setcap' (grants cap_dac_read_search "
        "to the binary), or 'sudo cmake --build . --target install-udev' (udev "
        "rule for the energy_uj files), or run geth as root";
    return st;
  }

  // --- AMD hwmon sources, in table order (amd_energy before zenpower) --------
  const std::string hwmon = _root + "/class/hwmon";
  for (const auto source : cpu_power_counter_sources()) {
    if (source == CpuPackagePowerSource::kAmdEnergy && probe_amd_energy(st, hwmon, ec))
      return st;
    if (source == CpuPackagePowerSource::kZenpower && probe_zenpower(st, hwmon, ec))
      return st;
  }

  st.detail = "no RAPL package domain and no amd_energy/zenpower hwmon driver found";
  if (cpu_vendor_is_amd())
    st.detail +=
        "\n  if an integrated AMD GPU exists, its PPT supplies CPU package"
        " power (APU package power includes iGPU — see GPU section)";
  // Deliberately no module-loading hint: zenpower is an out-of-tree module
  // that requires k10temp to be unloaded first, and amd_energy is gone from
  // mainline kernels — suggesting either would be misleading.
  return st;
}

bool CpuPowerSource::probe_amd_energy(SourceStatus& st, const std::string& hwmon,
                                      std::error_code& ec) {
  // amd_energy (Documentation/hwmon/amd_energy.rst): energyN_input is a
  // cumulative microjoule counter, named by energyN_label — "EcoreNNN"
  // (per core) or "EsocketN" (per socket). Only the Esocket* entries measure
  // package power, so only those are summed. The driver publishes no counter
  // range: a backwards step is a reset (sample skipped, baseline re-anchored).
  if (!std::filesystem::is_directory(hwmon, ec)) return false;
  for (auto it = std::filesystem::directory_iterator(hwmon, ec);
       it != std::filesystem::directory_iterator(); it.increment(ec)) {
    const std::filesystem::path dev = it->path();
    if (!it->is_directory(ec) || read_first_line(dev / "name") != "amd_energy") continue;

    // Collect the (index, input path) of every energyN_input present.
    std::vector<std::pair<unsigned, std::filesystem::path>> entries;
    auto eit = std::filesystem::directory_iterator(dev, ec);
    for (; eit != std::filesystem::directory_iterator(); eit.increment(ec)) {
      unsigned n = 0;
      if (!parse_energy_index(eit->path().filename().string(), n)) continue;
      const std::string label =
          read_first_line(dev / ("energy" + std::to_string(n) + "_label"));
      if (label.rfind("Esocket", 0) != 0) continue;  // Ecore*: not package power
      entries.emplace_back(n, eit->path());
    }
    if (entries.empty()) continue;  // no Esocket* counter: not a usable driver

    std::sort(entries.begin(), entries.end());
    for (const auto& [n, input] : entries) {
      (void)n;
      Domain d;
      int err = 0;
      if (!SysfsFile::open(input.string(), d.energy, err)) continue;  // unreadable: skip it
      _domains.push_back(std::move(d));
      if (st.paths.size() < 4) st.paths.push_back(input);
    }
    if (_domains.empty()) continue;
    _backend = CpuPackagePowerSource::kAmdEnergy;
    st.available = true;
    st.detail = "amd_energy hwmon: " + std::to_string(_domains.size()) +
                " Esocket* energy counter(s) (cumulative microjoules, 64-bit"
                " accumulator — a decrease is a reset, not a wrap)" +
                chain_line();
    return true;
  }
  return false;
}

bool CpuPowerSource::probe_zenpower(SourceStatus& st, const std::string& hwmon,
                                    std::error_code& ec) {
  // zenpower (out-of-tree module; layout per its README): two INSTANTANEOUS
  // microwatt inputs,
  //   power1_input (label SVI2_P_Core)  — the CPU cores
  //   power2_input (label SVI2_P_SoC)   — the SoC
  // Package power is their sum; there is no counter math.
  if (!std::filesystem::is_directory(hwmon, ec)) return false;
  for (auto it = std::filesystem::directory_iterator(hwmon, ec);
       it != std::filesystem::directory_iterator(); it.increment(ec)) {
    const std::filesystem::path dev = it->path();
    if (!it->is_directory(ec) || read_first_line(dev / "name") != "zenpower") continue;

    SysfsFile p1, p2;
    int err = 0;
    if (!SysfsFile::open((dev / "power1_input").string(), p1, err)) continue;
    if (!SysfsFile::open((dev / "power2_input").string(), p2, err)) continue;
    _z_core = std::move(p1);
    _z_soc = std::move(p2);

    const std::string l1 = read_first_line(dev / "power1_label");
    const std::string l2 = read_first_line(dev / "power2_label");
    _backend = CpuPackagePowerSource::kZenpower;
    st.available = true;
    st.detail = "zenpower hwmon: instantaneous microwatts (" +
                (l1.empty() ? "SVI2_P_Core" : l1) + " + " +
                (l2.empty() ? "SVI2_P_SoC" : l2) + ")" + chain_line();
    if (st.paths.size() < 4) st.paths.push_back(_z_core.path());
    if (st.paths.size() < 4) st.paths.push_back(_z_soc.path());
    return true;
  }
  return false;
}

void CpuPowerSource::sample(Snapshot& out) {
  const TimePoint now = Clock::now();

  // zenpower: instantaneous microwatts — sum the two inputs, no baseline.
  if (_backend == CpuPackagePowerSource::kZenpower) {
    const auto core = parse::to_u64(parse::trim(_z_core.read()));
    const auto soc = parse::to_u64(parse::trim(_z_soc.read()));
    if (!core || !soc) return;  // driver hiccup: skip this tick
    out.cpu.package_power_w =
        (static_cast<double>(*core) + static_cast<double>(*soc)) / 1e6;
    out.cpu.package_power_source = _backend;
    return;
  }

  // RAPL / amd_energy: cumulative microjoule counters, power = Δ/Δt.
  if (_domains.empty()) return;
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
  bool reanchor = false;
  for (std::size_t i = 0; i < _domains.size(); ++i) {
    unsigned long long range = 0;
    if (_domains[i].range.valid()) {
      if (const auto r = parse::to_u64(parse::trim(_domains[i].range.read()))) range = *r;
    }
    const auto delta = counter_delta(vals[i], _domains[i].prev_uj, range);
    if (!delta) {
      // A reset (amd_energy going backwards) or a fault (RAPL advancing more
      // than a full scale): this tick is untrustworthy.
      reanchor = true;
      break;
    }
    total_uj += static_cast<double>(*delta);
  }
  if (reanchor) {
    // Re-anchor every baseline to the current values so the NEXT tick diffs
    // against a coherent state — without this, a reset would skip samples
    // forever (the counters never climb back past the stale baseline).
    for (std::size_t i = 0; i < _domains.size(); ++i) _domains[i].prev_uj = vals[i];
    _prev_tp = now;
    return;  // skip this sample
  }
  for (std::size_t i = 0; i < _domains.size(); ++i) _domains[i].prev_uj = vals[i];

  // Real elapsed time between samples, not the nominal interval: the tick
  // period drifts, and the machine may have just woken from suspend.
  const double dt = std::chrono::duration<double>(now - _prev_tp).count();
  if (dt > 0.0 && dt < 604800.0) {  // >1 week elapsed: stale baseline, skip
    out.cpu.package_power_w = total_uj / 1e6 / dt;  // microjoules -> joules -> watts
    out.cpu.package_power_source = _backend;
  }
  _prev_tp = now;
}

}  // namespace geth
