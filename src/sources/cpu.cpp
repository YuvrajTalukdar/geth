#include "sources/cpu.hpp"

#include "util/parse.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

namespace geth {

void parse_proc_stat(std::string_view text, std::vector<CpuTimes>& out) {
  out.clear();
  parse::for_each_line(text, [&](std::string_view line) {
    // Only the aggregate ("cpu ") and per-core ("cpu0", ...) lines; this
    // skips "ctxt", "intr", etc.
    if (line.size() < 4 || !line.starts_with("cpu")) return true;
    const char c = line[3];
    const bool aggregate = (c == ' ');
    const bool core = (c >= '0' && c <= '9');
    if (!aggregate && !core) return true;

    // "cpu"/"cpuN" + 8 time fields, hand-tokenized: no intermediate vectors
    // on the per-tick hot path.
    std::string_view rest = line;
    const auto sp0 = rest.find(' ');
    if (sp0 == std::string_view::npos) return true;
    rest = rest.substr(sp0 + 1);
    unsigned long long f[8];
    int nf = 0;
    while (nf < 8) {
      while (!rest.empty() && rest.front() == ' ') rest = rest.substr(1);
      const auto sp = rest.find(' ');
      const std::string_view tok =
          (sp == std::string_view::npos) ? rest : rest.substr(0, sp);
      const auto v = parse::to_u64(tok);
      if (!v) return true;  // malformed line: skip
      f[nf++] = *v;
      if (sp == std::string_view::npos) break;
      rest = rest.substr(sp + 1);
    }
    if (nf != 8) return true;
    CpuTimes t;
    t.user = f[0];
    t.nice = f[1];
    t.system = f[2];
    t.idle = f[3];
    t.iowait = f[4];
    t.irq = f[5];
    t.softirq = f[6];
    t.steal = f[7];
    out.push_back(t);
    return true;
  });
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

void parse_cpuinfo_mhz(std::string_view text, std::vector<double>& out) {
  out.clear();
  parse::for_each_line(text, [&](std::string_view line) {
    if (!line.starts_with("cpu MHz")) return true;
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) return true;
    if (const auto v = parse::to_double(line.substr(colon + 1))) out.push_back(*v);
    return true;
  });
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

std::size_t CpuSource::read_frequency() {
  // Fills the first n entries of _mhz (compact: only readable cores),
  // returns n. _mhz keeps its capacity, so this never allocates after the
  // first tick.
  std::size_t n = 0;
  if (!_cores.empty()) {
    if (_mhz.size() < _cores.size()) _mhz.resize(_cores.size());
    for (auto& f : _cores) {
      const auto v = parse::to_u64(parse::trim(f.read()));
      if (v) _mhz[n++] = static_cast<double>(*v) / 1000.0;
    }
  } else if (_cpuinfo.valid()) {
    parse_cpuinfo_mhz(_cpuinfo.read(), _mhz);
    n = _mhz.size();
  }
  return n;
}

void CpuSource::sample(Snapshot& out) {
  parse_proc_stat(_stat.read(), _cur);
  if (_cur.empty()) return;  // unreadable this tick: keep the previous values
  if (!_sized) {
    // One-time: baseline counters, per-core slots in the reused Snapshot.
    _prev = _cur;
    out.cpu.per_core_pct.resize(_cur.size() - 1);
    _sized = true;
  }
  if (_prev.size() == _cur.size()) {
    for (std::size_t i = 0; i < _cur.size(); ++i) {
      const auto pct = usage_pct_between(_prev[i], _cur[i]);
      if (i == 0)
        out.cpu.usage_pct = pct;
      else
        out.cpu.per_core_pct[i - 1] = pct;
    }
    std::swap(_prev, _cur);
  }

  // Frequency: prefer per-core scaling_cur_freq (kHz); fall back to cpuinfo.
  const std::size_t n = read_frequency();
  if (n > 0) {
    double sum = 0.0;
    double mx = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      sum += _mhz[i];
      mx = std::max(mx, _mhz[i]);
    }
    out.cpu.freq_avg_mhz = sum / static_cast<double>(n);
    out.cpu.freq_max_mhz = mx;
  }
}

}  // namespace geth
