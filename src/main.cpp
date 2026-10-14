// GETH — Geth Engine for Tracking Hardware
// Phase 1: data collection layer with text + JSON output (no UI yet).

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <fstream>
#include <string>
#include <sys/utsname.h>
#include <unistd.h>

#include "core/collector.hpp"
#include "sources/cpu.hpp"
#include "sources/cpu_power.hpp"
#include "sources/demo.hpp"
#include "sources/disk.hpp"
#include "sources/gpu_registry.hpp"
#include "sources/memory.hpp"
#include "sources/net.hpp"
#include "sources/temps.hpp"
#include "util/format.hpp"
#include "util/parse.hpp"

using namespace geth;

namespace {

constexpr const char* kFullName = "GETH — Geth Engine for Tracking Hardware";

std::atomic<bool> g_stop{false};
void on_signal(int) {
  g_stop = true;  // async-signal-safe: a plain atomic store
}

struct Options {
  bool probe = false;
  bool dump = false;  // default live mode
  bool json = false;
  bool demo = false;
  double interval = 1.0;
  unsigned long long count = 0;  // 0 = until Ctrl-C
};

void print_help() {
  std::printf("%s %s\n", kFullName, GETH_VERSION);
  std::printf(
      "A fast, lightweight terminal system monitor for Linux.\n"
      "(Phase 1: data collection layer — no UI yet.)\n\n"
      "Usage: geth [options]\n"
      "  --probe           List every data source, its availability and fix hints\n"
      "  --dump            Print human-readable samples (default mode)\n"
      "  --json            Print samples as JSON (one object per sample)\n"
      "  --demo            Use synthetic data; no hardware is accessed\n"
      "  --interval <sec>  Sampling interval in seconds (default 1.0, min 0.1)\n"
      "  --count <n>       Stop after n samples (default: run until Ctrl-C)\n"
      "  --help            Show this help\n"
      "  --version         Print name and version\n");
}

bool parse_double(const char* s, double& out) {
  char* end = nullptr;
  const double v = std::strtod(s, &end);
  if (end == s || *end != '\0' || v < 0.0) return false;
  out = v;
  return true;
}

bool parse_u64_arg(const char* s, unsigned long long& out) {
  char* end = nullptr;
  const unsigned long long v = std::strtoull(s, &end, 10);
  if (end == s || *end != '\0') return false;
  out = v;
  return true;
}

// Options: true  = flag found,  false = not found, -1 = bad value.
int parse_args(int argc, char** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    std::string val;
    const auto eq = a.find('=');
    if (eq != std::string::npos) {
      val = a.substr(eq + 1);
      a = a.substr(0, eq);
    }
    auto need_value = [&](const char* what) -> const char* {
      if (!val.empty()) return val.c_str();
      if (i + 1 < argc) return argv[++i];
      std::fprintf(stderr, "geth: %s needs a value\n", what);
      return nullptr;
    };
    if (a == "--probe") {
      o.probe = true;
    } else if (a == "--dump") {
      o.dump = true;
    } else if (a == "--json") {
      o.json = true;
    } else if (a == "--demo") {
      o.demo = true;
    } else if (a == "--interval") {
      const char* v = need_value("--interval");
      if (!v || !parse_double(v, o.interval)) return -1;
      if (o.interval < 0.1) o.interval = 0.1;
    } else if (a == "--count") {
      const char* v = need_value("--count");
      if (!v || !parse_u64_arg(v, o.count)) return -1;
    } else if (a == "--help" || a == "-h") {
      print_help();
      std::exit(0);
    } else if (a == "--version") {
      std::printf("%s %s\n", kFullName, GETH_VERSION);
      std::exit(0);
    } else {
      std::fprintf(stderr, "geth: unknown option '%s' (try --help)\n", argv[i]);
      return -1;
    }
  }
  return 0;
}

void sleep_interruptible(double seconds) {
  const long long ns = static_cast<long long>(seconds * 1e9);
  struct timespec ts;
  ts.tv_sec = static_cast<time_t>(ns / 1000000000LL);
  ts.tv_nsec = ns % 1000000000LL;
  while (!g_stop && nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    // Interrupted: the kernel left the remaining time in ts; loop unless a
    // signal we care about arrived (g_stop).
  }
}

// ---------------------------------------------------------------------------
// Human-readable output (--dump)
// ---------------------------------------------------------------------------

void print_dump(const Snapshot& s) {
  using namespace format;
  auto opt = [](const std::optional<double>& v) { return v.has_value() ? "yes" : "n/a"; };
  (void)opt;

  // The source suffix is shown for APU PPT — the one case where the figure
  // is not plain CPU package power (it includes the iGPU, so the provenance
  // belongs on the line).
  std::string cpu_power = or_na(s.cpu.package_power_w, watts);
  if (s.cpu.package_power_source == CpuPackagePowerSource::kApuPpt) cpu_power += " (APU PPT)";
  std::printf("CPU      usage %-8s  freq %s / max %s   temp %-9s  power %s\n",
              or_na(s.cpu.usage_pct, percent).c_str(), or_na(s.cpu.freq_avg_mhz, freq).c_str(),
              or_na(s.cpu.freq_max_mhz, freq).c_str(), or_na(s.cpu.temp_c, celsius).c_str(),
              cpu_power.c_str());

  std::string mem_line =
      "used " + or_na(s.mem.ram_used_bytes, bytes) + " / " + or_na(s.mem.ram_total_bytes, bytes);
  if (s.mem.swap_total_bytes)
    mem_line += "   swap " + or_na(s.mem.swap_used_bytes, bytes) + " / " +
                or_na(s.mem.swap_total_bytes, bytes);
  std::printf("Memory   %s\n", mem_line.c_str());

  for (const auto& g : s.gpus) {
    // Integrated cards get the deciding method in the bracket so the flag is
    // never a mystery; discrete cards keep the line short.
    std::string tag = g.pci_bus_id;
    if (g.is_integrated && !g.integrated_method.empty())
      tag += ", " + g.integrated_method;
    std::printf("GPU %-4s %s   %s   [%s]\n", g.id.c_str(), g.name.c_str(), to_string(g.state),
                tag.c_str());
    std::printf("         usage %-6s  VRAM %-9s/%-9s  clocks %s / %s   %s   %s\n",
                or_na(g.usage_pct, percent).c_str(), or_na(g.vram_used_bytes, bytes).c_str(),
                or_na(g.vram_total_bytes, bytes).c_str(), or_na(g.core_clock_mhz, freq).c_str(),
                or_na(g.mem_clock_mhz, freq).c_str(), or_na(g.power_w, watts).c_str(),
                or_na(g.temp_c, celsius).c_str());
  }

  if (!s.disk.devices.empty()) {
    std::string line;
    for (const auto& d : s.disk.devices) {
      line += d.name + "  R " + or_na(d.read_bytes_per_s, rate) + "  W " +
              or_na(d.write_bytes_per_s, rate) + "    ";
    }
    std::printf("Disk     %s", line.c_str());
    std::printf(" | total R %-11s W %s\n", or_na(s.disk.total_read_bytes_per_s, rate).c_str(),
                or_na(s.disk.total_write_bytes_per_s, rate).c_str());
  }

  if (!s.net.interfaces.empty()) {
    std::string line;
    for (const auto& n : s.net.interfaces) {
      line += n.name + "  rx " + or_na(n.rx_bytes_per_s, rate) + "  tx " +
              or_na(n.tx_bytes_per_s, rate) + "    ";
    }
    std::printf("Network  %s", line.c_str());
    std::printf(" | total rx %-10s tx %s\n", or_na(s.net.total_rx_bytes_per_s, rate).c_str(),
                or_na(s.net.total_tx_bytes_per_s, rate).c_str());
  }
}

// ---------------------------------------------------------------------------
// JSON output (--json)
// ---------------------------------------------------------------------------

std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 2);
  out += '"';
  for (char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out += c;
        }
    }
  }
  out += '"';
  return out;
}

std::string jnum(double v) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%.2f", v);
  return buf;
}

void jopt(std::string& o, const char* key, const std::optional<double>& v, bool comma) {
  o += "\"";
  o += key;
  o += "\":";
  o += v ? jnum(*v) : std::string("null");
  if (comma) o += ",";
}

void jbytes(std::string& o, const char* key, const std::optional<double>& v, bool comma) {
  o += "\"";
  o += key;
  o += "\":";
  char buf[40];
  if (v) {
    std::snprintf(buf, sizeof(buf), "%.0f", *v);
    o += buf;
  } else {
    o += "null";
  }
  if (comma) o += ",";
}

std::string to_json(const Snapshot& s) {
  std::string o;
  o += "{";
  char tbuf[40];
  std::snprintf(tbuf, sizeof(tbuf), "%.3f",
                std::chrono::duration<double>(s.time.time_since_epoch()).count());
  o += "\"t\":";
  o += tbuf;
  o += ",\"cpu\":{";
  jopt(o, "usage_pct", s.cpu.usage_pct, !s.cpu.per_core_pct.empty());
  if (!s.cpu.per_core_pct.empty()) {
    o += "\"per_core\":[";
    for (std::size_t i = 0; i < s.cpu.per_core_pct.size(); ++i) {
      if (i) o += ",";
      o += s.cpu.per_core_pct[i] ? jnum(*s.cpu.per_core_pct[i]) : "null";
    }
    o += "],";
  }
  jopt(o, "freq_avg_mhz", s.cpu.freq_avg_mhz, true);
  jopt(o, "freq_max_mhz", s.cpu.freq_max_mhz, true);
  jopt(o, "temp_c", s.cpu.temp_c, true);
  jopt(o, "package_power_w", s.cpu.package_power_w, true);
  o += "\"package_power_source\":";
  o += s.cpu.package_power_source
           ? json_escape(to_string(*s.cpu.package_power_source))
           : std::string("null");
  o += "},\"mem\":{";
  jbytes(o, "ram_total_bytes", s.mem.ram_total_bytes, true);
  jbytes(o, "ram_used_bytes", s.mem.ram_used_bytes, true);
  jbytes(o, "swap_total_bytes", s.mem.swap_total_bytes, true);
  jbytes(o, "swap_used_bytes", s.mem.swap_used_bytes, false);
  o += "},\"gpus\":[";
  for (std::size_t i = 0; i < s.gpus.size(); ++i) {
    const auto& g = s.gpus[i];
    if (i) o += ",";
    o += "{";
    o += "\"id\":";
    o += json_escape(g.id);
    o += ",\"name\":";
    o += json_escape(g.name);
    o += ",\"vendor\":";
    o += json_escape(to_string(g.vendor));
    o += ",\"pci\":";
    o += json_escape(g.pci_bus_id);
    o += ",\"integrated\":";
    o += g.is_integrated ? "true" : "false";
    o += ",\"integrated_method\":";
    o += json_escape(g.integrated_method);
    o += ",\"state\":";
    o += json_escape(to_string(g.state));
    o += ",";
    jopt(o, "usage_pct", g.usage_pct, true);
    jbytes(o, "vram_used_bytes", g.vram_used_bytes, true);
    jbytes(o, "vram_total_bytes", g.vram_total_bytes, true);
    jopt(o, "core_clock_mhz", g.core_clock_mhz, true);
    jopt(o, "mem_clock_mhz", g.mem_clock_mhz, true);
    jopt(o, "power_w", g.power_w, true);
    jopt(o, "ppt_w", g.ppt_w, true);  // raw PPT (integrated GPUs: package power)
    jopt(o, "temp_c", g.temp_c, false);
    o += "}";
  }
  o += "],\"disk\":{";
  o += "\"devices\":[";
  for (std::size_t i = 0; i < s.disk.devices.size(); ++i) {
    const auto& d = s.disk.devices[i];
    if (i) o += ",";
    o += "{\"name\":";
    o += json_escape(d.name);
    o += ",";
    jbytes(o, "read_bps", d.read_bytes_per_s, true);
    jbytes(o, "write_bps", d.write_bytes_per_s, false);
    o += "}";
  }
  o += "],";
  jbytes(o, "total_read_bps", s.disk.total_read_bytes_per_s, true);
  jbytes(o, "total_write_bps", s.disk.total_write_bytes_per_s, false);
  o += "},\"net\":{";
  o += "\"interfaces\":[";
  for (std::size_t i = 0; i < s.net.interfaces.size(); ++i) {
    const auto& n = s.net.interfaces[i];
    if (i) o += ",";
    o += "{\"name\":";
    o += json_escape(n.name);
    o += ",";
    jbytes(o, "rx_bps", n.rx_bytes_per_s, true);
    jbytes(o, "tx_bps", n.tx_bytes_per_s, false);
    o += "}";
  }
  o += "],";
  jbytes(o, "total_rx_bps", s.net.total_rx_bytes_per_s, true);
  jbytes(o, "total_tx_bps", s.net.total_tx_bytes_per_s, false);
  o += "}}";
  return o;
}

// ---------------------------------------------------------------------------

// --probe system info header: kernel, CPU model, cpufreq driver, NVIDIA
// driver version, pci.ids path.
void print_system_header(GpuRegistrySource* gpu_registry) {
  std::printf("%s %s\n", kFullName, GETH_VERSION);

  std::string kernel = "unknown";
  utsname u;
  if (uname(&u) == 0) kernel = std::string(u.sysname) + " " + u.release;
  std::printf("  kernel          %s\n", kernel.c_str());

  // CPU model: /proc/cpuinfo "model name" — the hardware's own label.
  std::string model = "unknown";
  {
    std::ifstream f("/proc/cpuinfo");
    std::string line;
    while (std::getline(f, line)) {
      if (const auto m = parse_cpuinfo_model_name(line)) {
        model = *m;
        break;
      }
    }
  }
  std::printf("  cpu             %s\n", model.c_str());

  std::string cpufreq = "n/a";
  {
    std::ifstream f("/sys/devices/system/cpu/cpu0/cpufreq/scaling_driver");
    std::getline(f, cpufreq);
    if (cpufreq.empty()) cpufreq = "n/a";
  }
  std::printf("  cpufreq driver  %s\n", cpufreq.c_str());

  // nvmlSystemGetDriverVersion only if the NVML session is already live;
  // otherwise /sys/module/nvidia/version (a file read — never wakes the GPU).
  std::string nvdrv;
  if (gpu_registry) nvdrv = gpu_registry->nvidia_driver_version();
  std::printf("  nvidia driver   %s\n", (nvdrv.empty() ? std::string("n/a") : nvdrv).c_str());

  std::string pciids = gpu_registry ? gpu_registry->pci_ids_source() : std::string("(n/a)");
  std::printf("  pci.ids         %s\n\n", pciids.c_str());
}

int run_probe(Collector& collector, GpuRegistrySource* gpu_registry) {
  collector.probe_all(/*warm_up=*/false);
  print_system_header(gpu_registry);
  for (const auto& s : collector.statuses()) {
    // detail may be multi-line (per-GPU evidence blocks, net exclusions):
    // the first line prints next to the status, the rest indented under it.
    const auto lines = parse::split_lines(s.detail);
    bool first = true;
    for (const auto& l : lines) {
      if (l.empty()) continue;
      if (first) {
        std::printf("%-22s %-12s %.*s\n", s.description.c_str(),
                    s.available ? "available" : "UNAVAILABLE",
                    static_cast<int>(l.size()), l.data());
        first = false;
      } else {
        std::printf("%*s%.*s\n", 36, "", static_cast<int>(l.size()), l.data());
      }
    }
    if (first) std::printf("%-22s %s\n", s.description.c_str(),
                           s.available ? "available" : "UNAVAILABLE");
    if (!s.available && !s.hint.empty())
      std::printf("%*s%s\n", 36, "", ("  fix: " + s.hint).c_str());
    if (!s.paths.empty()) {
      std::printf("%*s%s", 36, "", "  paths: ");
      for (std::size_t i = 0; i < s.paths.size() && i < 3; ++i)
        std::printf("%s%s", i ? ", " : "", s.paths[i].c_str());
      if (s.paths.size() > 3) std::printf(", … +%zu more", s.paths.size() - 3);
      std::printf("\n");
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Options opts;
  if (parse_args(argc, argv, opts) != 0) return 2;

  if (opts.json && opts.probe) {
    std::fprintf(stderr, "geth: --probe and --json are mutually exclusive\n");
    return 2;
  }

  Collector collector;
  std::unique_ptr<GpuRegistrySource> gpu_registry;
  GpuRegistrySource* registry = nullptr;
  if (opts.demo) {
    collector.add(std::make_unique<DemoSource>());
  } else {
    collector.add(std::make_unique<CpuSource>());
    collector.add(std::make_unique<CpuTempSource>());
    collector.add(std::make_unique<CpuPowerSource>());
    collector.add(std::make_unique<MemorySource>());
    // One registry source owns both backends (and the unclaimed-device
    // reporting); the backend list lives in gpu_registry.cpp.
    gpu_registry = std::make_unique<GpuRegistrySource>();
    registry = gpu_registry.get();
    collector.add(std::move(gpu_registry));
    collector.add(std::make_unique<DiskSource>());
    collector.add(std::make_unique<NetSource>());
  }

  if (opts.probe) return run_probe(collector, registry);

  collector.probe_all(/*warm_up=*/true);
  if (!collector.any_available()) {
    std::fprintf(stderr, "%s: no data source is available on this machine (see --probe)\n",
                 kFullName);
    return 1;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  unsigned long long done = 0;
  while (!g_stop && (opts.count == 0 || done < opts.count)) {
    const Snapshot snap = collector.sample();
    if (opts.json) {
      std::printf("%s\n", to_json(snap).c_str());
      std::fflush(stdout);
    } else {
      print_dump(snap);
    }
    ++done;
    if (g_stop || (opts.count != 0 && done >= opts.count)) break;
    if (opts.count == 0 || done < opts.count) sleep_interruptible(opts.interval);
  }
  return 0;
}
