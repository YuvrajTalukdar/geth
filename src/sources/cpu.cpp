#include "sources/cpu.hpp"

#include "util/parse.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace geth {

std::vector<CpuTimes> parse_proc_stat(std::string_view text) {
  std::vector<CpuTimes> out;
  for (const std::string_view line : parse::split_lines(text)) {
    // Only the aggregate ("cpu ") and per-core ("cpu0", ...) lines; this
    // skips "ctxt", "intr", etc.
    if (line.size() < 4 || !line.starts_with("cpu")) continue;
    const char c = line[3];
    const bool aggregate = (c == ' ');
    const bool core = (c >= '0' && c <= '9');
    if (!aggregate && !core) continue;

    const auto toks = parse::tokens(line);
    if (toks.size() < 9) continue;  // "cpu" + 8 time fields
    CpuTimes t;
    bool ok = true;
    auto get = [&](std::size_t i) -> unsigned long long {
      const auto v = parse::to_u64(toks[i]);
      if (!v) ok = false;
      return v ? *v : 0;
    };
    t.user = get(1);
    t.nice = get(2);
    t.system = get(3);
    t.idle = get(4);
    t.iowait = get(5);
    t.irq = get(6);
    t.softirq = get(7);
    t.steal = get(8);
    if (!ok) continue;
    out.push_back(t);
  }
  return out;
}

std::optional<double> usage_pct_between(const CpuTimes& prev, const CpuTimes& now) {
  // Unsigned subtraction is deliberate: these are monotonic kernel counters.
  const unsigned long long dtotal = now.total() - prev.total();
  const unsigned long long didle = now.idle_all() - prev.idle_all();
  if (dtotal == 0) return std::nullopt;
  if (didle > dtotal) return std::nullopt;  // a field wrapped: distrust the pair
  const unsigned long long dbusy = dtotal - didle;
  return 100.0 * static_cast<double>(dbusy) / static_cast<double>(dtotal);
}

std::vector<double> parse_cpuinfo_mhz(std::string_view text) {
  std::vector<double> out;
  for (const std::string_view line : parse::split_lines(text)) {
    if (!line.starts_with("cpu MHz")) continue;
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    const auto v = parse::to_double(line.substr(colon + 1));
    if (v) out.push_back(*v);
  }
  return out;
}

std::optional<std::string> parse_cpuinfo_model_name(std::string_view text) {
  for (const std::string_view line : parse::split_lines(text)) {
    if (!line.starts_with("model name")) continue;
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) continue;
    const auto name = parse::trim(line.substr(colon + 1));
    if (!name.empty()) return std::string(name);
  }
  return std::nullopt;
}

namespace {

std::vector<int> online_cores(const std::string& base) {
  std::vector<int> cores;
  if (!std::filesystem::is_directory(base)) return cores;
  for (auto it = std::filesystem::directory_iterator(base);
       it != std::filesystem::directory_iterator(); ++it) {
    const std::string n = it->path().filename().string();
    if (n.size() < 4 || n.compare(0, 3, "cpu") != 0) continue;
    bool all_digits = true;
    for (std::size_t i = 3; i < n.size(); ++i)
      if (n[i] < '0' || n[i] > '9') all_digits = false;
    if (!all_digits) continue;
    cores.push_back(std::atoi(n.c_str() + 3));
  }
  std::sort(cores.begin(), cores.end());
  return cores;
}

}  // namespace

SourceStatus CpuSource::probe() {
  SourceStatus st;
  st.description = name();
  int err = 0;
  if (!SysfsFile::open("/proc/stat", _stat, err)) {
    st.detail = "cannot open /proc/stat (errno " + std::to_string(err) + ")";
    st.hint = "this looks like a broken or unusual Linux userspace; nothing to fix in GETH";
    return st;
  }
  st.paths.push_back(_stat.path());

  // One scaling_cur_freq fd per online core (the value is in kHz).
  const std::string base = "/sys/devices/system/cpu";
  for (const int c : online_cores(base)) {
    SysfsFile f;
    int e = 0;
    const std::string path = base + "/cpu" + std::to_string(c) + "/cpufreq/scaling_cur_freq";
    if (!SysfsFile::open(path, f, e)) continue;  // offline core or no cpufreq
    if (st.paths.size() < 3) st.paths.push_back(f.path());
    _cores.push_back(std::move(f));
  }

  // /proc/cpuinfo: the "model name" line identifies the machine from the
  // hardware (never trust a spec sheet), and the "cpu MHz" lines are the
  // frequency fallback when cpufreq is absent.
  int e = 0;
  std::string model;
  if (SysfsFile::open("/proc/cpuinfo", _cpuinfo, e)) {
    if (st.paths.size() < 3) st.paths.push_back("/proc/cpuinfo");
    if (const auto m = parse_cpuinfo_model_name(_cpuinfo.read())) model = *m;
  }

  if (_cores.empty()) {
    st.detail = "no cpufreq sysfs";
    if (_cpuinfo.valid()) {
      st.detail += "; frequency falls back to 'cpu MHz' lines in /proc/cpuinfo";
    } else {
      st.detail += "; no per-core frequency source found (cpufreq driver not loaded?)";
      st.hint =
          "load the cpufreq driver: sudo modprobe cpufreq_pstate (AMD) or cpufreq_conservative";
    }
  } else {
    st.detail = "frequency from cpufreq scaling_cur_freq (" +
                std::to_string(_cores.size()) + " logical CPUs)";
  }
  if (!model.empty()) st.detail = model + " — " + st.detail;
  st.available = true;
  return st;
}

void CpuSource::sample(Snapshot& out) {
  const auto now = parse_proc_stat(_stat.read());
  if (!now.empty() && _prev.size() == now.size()) {
    for (std::size_t i = 0; i < now.size(); ++i) {
      const auto pct = usage_pct_between(_prev[i], now[i]);
      if (i == 0)
        out.cpu.usage_pct = pct;
      else
        out.cpu.per_core_pct.push_back(pct);
    }
  }
  _prev = now;

  // Frequency: prefer per-core scaling_cur_freq (kHz); fall back to cpuinfo.
  std::vector<double> mhz;
  if (!_cores.empty()) {
    for (auto& f : _cores) {
      const auto v = parse::to_u64(parse::trim(f.read()));
      if (v) mhz.push_back(static_cast<double>(*v) / 1000.0);
    }
  } else if (_cpuinfo.valid()) {
    mhz = parse_cpuinfo_mhz(_cpuinfo.read());
  }
  if (!mhz.empty()) {
    double sum = 0.0;
    double mx = 0.0;
    for (double x : mhz) {
      sum += x;
      mx = std::max(mx, x);
    }
    out.cpu.freq_avg_mhz = sum / static_cast<double>(mhz.size());
    out.cpu.freq_max_mhz = mx;
  }
}

}  // namespace geth
