// GETH — Geth Engine for Tracking Hardware
// Phase 1: data collection layer with text + JSON output (no UI yet).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cerrno>
#include <chrono>
#include <fstream>
#include <random>
#include <string>
#include <sys/utsname.h>
#include <unistd.h>
#include <vector>

#include "core/collector.hpp"
#include "sources/cpu.hpp"
#include "sources/cpu_power.hpp"
#include "sources/demo.hpp"
#include "sources/disk.hpp"
#include "sources/gpu_registry.hpp"
#include "sources/memory.hpp"
#include "sources/net.hpp"
#include "sources/temps.hpp"
#include "ui/app.hpp"
#include "ui/layout.hpp"
#include "ui/panel_spec.hpp"
#include "ui/term_probe.hpp"
#include "ui/theme.hpp"
#include "util/alloc_count.hpp"
#include "util/format.hpp"
#include "util/parse.hpp"
#include "util/stop.hpp"
#include "util/terminal.hpp"
#include "util/trace.hpp"

using namespace geth;

namespace {

constexpr const char* kFullName = "GETH — Geth Engine for Tracking Hardware";

void on_signal(int) {
  util::g_stop = true;  // async-signal-safe: a plain atomic store
}

struct Options {
  bool probe = false;
  bool dump = false;  // text samples; the UI is the default when nothing is set
  bool json = false;
  bool demo = false;
  long long interval_ms = 1000;  // sampling interval in milliseconds
  bool interval_explicit = false;  // --interval was passed; beats the config
  unsigned long long count = 0;  // 0 = until Ctrl-C
  std::string config_path;  // --config PATH ("" = the XDG default location)
  bool no_save = false;      // --no-save: the config file is read-only
  std::string script;        // --render-frame --script: synthetic events
  // Part B (UI) options
  bool ansi = false;          // --render-frame: emit full SGR escape codes
  bool stats = false;         // print render/perf stats on exit
  unsigned long long warmup = 60;  // --render-frame: samples to collect
  bool render_frame = false;  // --render-frame COLSxROWS
  std::string render_size;    // "COLSxROWS"
  bool render_png = false;     // --render-png COLSxROWS
  std::string render_png_size; // "COLSxROWS" for --render-png
  std::string color_mode;      // "" = auto, else truecolor|256|16
  int columns = 0;            // --columns N: pin the layout column count (0 = auto)
  bool layout_report = false;  // --layout-report COLSxROWS
  std::string layout_size;    // "COLSxROWS" for --layout-report
  bool perf = false;          // --perf: relayout + layout-cache benchmarks
  std::string theme;          // --theme NAME ("" = the config's [general] theme)
  bool list_themes = false;   // --list-themes
  bool probe_terminal = false;  // --probe-terminal: terminal capabilities, exit
  std::string renderer;      // --renderer pixels|text ("" = the config's value)
  std::string trace_series;   // --trace SERIES_ID: per-tick stage trace (off if empty)
};

void print_help() {
  std::printf("%s %s\n", kFullName, GETH_VERSION);
  std::printf(
      "A fast, lightweight terminal system monitor for Linux.\n"
      "With no mode flag, the interactive UI is started.\n\n"
      "Usage: geth [options]\n"
      "  (none)            Start the interactive dashboard UI\n"
      "  --probe           List every data source, its availability and fix hints\n"
      "  --dump            Print human-readable samples\n"
      "  --json            Print samples as JSON (one object per sample)\n"
      "  --demo            Use synthetic data; no hardware is accessed\n"
      "  --interval <ms>   Sampling interval in milliseconds (default 1000, min 10)\n"
      "  --count <n>       Stop after n samples (default: run until Ctrl-C)\n"
      "  --trace <id>      Per-tick stage trace of one series (raw/parse/lift/\n"
      "                    store/panel), e.g. --trace cpu.power\n"
      "  --render-frame COLSxROWS   Render one frame at that size and exit\n"
      "      [--warmup <n>]  samples to collect first (default 60; no sleep in\n"
      "                      --demo, real interval otherwise)\n"
      "      [--ansi]        emit full SGR escape codes instead of plain text\n"
      "  --render-png COLSxROWS     Render one frame to an 8-bit RGB PNG on\n"
      "                             stdout (background + text glyphs + the pixel\n"
      "                             rasters with --renderer pixels); always truecolor\n"
      "  --layout-report COLSxROWS  Print the layout decision (candidates, per-\n"
      "                             panel rects/modes) at that size and exit\n"
      "  --perf            Benchmark relayout + layout-cache reuse and exit\n"
      "  --columns <n>       Pin the layout column count (UI/render/report)\n"
      "  --stats             Print render/perf statistics on exit\n"
      "  --color <mode>      truecolor | 256 | 16 (default: auto-detect)\n"
      "  --config <path>     Path to the config file (default: $XDG_CONFIG_HOME/\n"
      "                      geth/config.toml, then ~/.config/geth/config.toml)\n"
      "  --theme <name>      Theme (built-in or custom file; see --list-themes;\n"
      "                      default: the config's [general] theme)\n"
      "  --list-themes       List the selectable themes (built-in + custom) and\n"
      "                      mark the active one; does not touch the hardware\n"
      "  --probe-terminal    Report this terminal's image-protocol support\n"
      "                      (kitty/sixel), the cell pixel size, and the graph\n"
      "                      renderer that will be chosen; does not touch the\n"
      "                      hardware\n"
      "  --renderer <name>   pixels | text — session override for the graph\n"
      "                      renderer (default: the config's graph_renderer)\n"
      "  --no-save           Never write the config file (read-only)\n"
      "  --script <events>   --render-frame only: replay a comma-separated list\n"
      "                      of synthetic events (keys, enter, space, tab,\n"
      "                      up/down/left/right, click:X:Y, ...) before the\n"
      "                      single frame is rendered\n"
      "  --help            Show this help\n"
      "  --version         Print name and version\n");
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
      // Milliseconds (spec): default 1000. --interval 10 == 10 ms.
      unsigned long long ms = 0;
      const char* v = need_value("--interval");
      if (!v || !parse_u64_arg(v, ms)) return -1;
      if (ms < 10) ms = 10;
      o.interval_ms = static_cast<long long>(ms);
      o.interval_explicit = true;
    } else if (a == "--count") {
      const char* v = need_value("--count");
      if (!v || !parse_u64_arg(v, o.count)) return -1;
    } else if (a == "--trace") {
      const char* v = need_value("--trace");
      if (!v) return -1;
      o.trace_series = v;
    } else if (a == "--ansi") {
      o.ansi = true;
    } else if (a == "--stats") {
      o.stats = true;
    } else if (a == "--warmup") {
      unsigned long long n = 0;
      const char* v = need_value("--warmup");
      if (!v || !parse_u64_arg(v, n)) return -1;
      o.warmup = n;
    } else if (a == "--render-frame") {
      const char* v = need_value("--render-frame");
      if (!v) return -1;
      o.render_frame = true;
      o.render_size = v;
    } else if (a == "--render-png") {
      const char* v = need_value("--render-png");
      if (!v) return -1;
      o.render_png = true;
      o.render_png_size = v;
    } else if (a == "--color") {
      const char* v = need_value("--color");
      if (!v) return -1;
      o.color_mode = v;
    } else if (a == "--config") {
      const char* v = need_value("--config");
      if (!v) return -1;
      o.config_path = v;
    } else if (a == "--theme") {
      const char* v = need_value("--theme");
      if (!v) return -1;
      o.theme = v;
    } else if (a == "--list-themes") {
      o.list_themes = true;
    } else if (a == "--probe-terminal") {
      o.probe_terminal = true;
    } else if (a == "--renderer") {
      const char* v = need_value("--renderer");
      if (!v) return -1;
      if (std::string(v) != "pixels" && std::string(v) != "text") {
        std::fprintf(stderr, "geth: --renderer must be pixels or text\n");
        return -1;
      }
      o.renderer = v;
    } else if (a == "--no-save") {
      o.no_save = true;
    } else if (a == "--script") {
      const char* v = need_value("--script");
      if (!v) return -1;
      o.script = v;
    } else if (a == "--columns") {
      unsigned long long n = 0;
      const char* v = need_value("--columns");
      if (!v || !parse_u64_arg(v, n)) return -1;
      o.columns = static_cast<int>(n);
    } else if (a == "--layout-report") {
      const char* v = need_value("--layout-report");
      if (!v) return -1;
      o.layout_report = true;
      o.layout_size = v;
    } else if (a == "--perf") {
      o.perf = true;
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
  while (!util::g_stop && nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    // Interrupted: the kernel left the remaining time in ts; loop unless a
    // signal we care about arrived (g_stop).
  }
}

// The full "model name" for the UI title. --demo uses the dev machine's own
// CPU so the header reads like the spec mock.
std::string read_cpu_model(bool demo) {
  if (demo) return "AMD Ryzen 5 5600H with Radeon Graphics";
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (std::getline(f, line)) {
    if (const auto m = parse_cpuinfo_model_name(line)) return *m;
  }
  return "unknown CPU";
}

// "COLSxROWS" (a lowercase or uppercase x). Rejects empty/zero/too-large.
bool parse_size(const std::string& s, int& W, int& H) {
  const auto x = s.find_first_of("xX");
  if (x == std::string::npos || x == 0 || x + 1 >= s.size()) return false;
  unsigned long long wv = 0, hv = 0;
  if (!parse_u64_arg(s.substr(0, x).c_str(), wv)) return false;
  if (!parse_u64_arg(s.substr(x + 1).c_str(), hv)) return false;
  if (wv < 1 || hv < 1 || wv > 10000 || hv > 10000) return false;
  W = static_cast<int>(wv);
  H = static_cast<int>(hv);
  return true;
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
  if (trace::matches("cpu.power"))
    trace::stage("panel", "cpu.power", "power: " + cpu_power);

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
    // GTT is printed only when the driver exposes it (absent on many APUs).
    std::string gtt;
    if (g.gtt_used_bytes || g.gtt_total_bytes)
      gtt = "  GTT " + or_na(g.gtt_used_bytes, bytes) + "/" + or_na(g.gtt_total_bytes, bytes);
    std::printf("         usage %-6s  VRAM %-9s/%-9s%s  clocks %s / %s   %s   %s\n",
                or_na(g.usage_pct, percent).c_str(), or_na(g.vram_used_bytes, bytes).c_str(),
                or_na(g.vram_total_bytes, bytes).c_str(), gtt.c_str(),
                or_na(g.core_clock_mhz, freq).c_str(), or_na(g.mem_clock_mhz, freq).c_str(),
                or_na(g.power_w, watts).c_str(), or_na(g.temp_c, celsius).c_str());
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
  if (!std::isfinite(v)) return "null";  // a clock-jump gap blanks rates to NaN
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
    o += ",\"pci_bus_id\":";
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
    jbytes(o, "gtt_used_bytes", g.gtt_used_bytes, true);
    jbytes(o, "gtt_total_bytes", g.gtt_total_bytes, true);
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

  // Cross-check (A6): when RAPL is actually readable (root, or after the
  // udev rule), its package power and the APU PPT are two independent
  // measurements of (nearly) the same thing and should agree within a few
  // watts. Average RAPL over ~0.7 s and print both, so a freshly installed
  // rule can be verified at a glance.
  bool rapl_up = false;
  for (const auto& s : collector.statuses())
    if (s.description == "CPU power" && s.available &&
        s.detail.find("RAPL") != std::string::npos)
      rapl_up = true;
  if (rapl_up) {
    collector.sample();  // baseline for the RAPL counter
    sleep_interruptible(0.7);
    const Snapshot& s = collector.sample();
    if (s.cpu.package_power_w &&
        s.cpu.package_power_source == CpuPackagePowerSource::kRapl) {
      for (const auto& g : s.gpus) {
        if (!g.is_integrated || !g.ppt_w) continue;
        std::printf("\nCross-check (expect within a few W under load):\n");
        std::printf("  RAPL package power  %s\n",
                    format::watts(*s.cpu.package_power_w).c_str());
        std::printf("  APU PPT (%s)       %s\n", g.pci_bus_id.c_str(),
                    format::watts(*g.ppt_w).c_str());
        break;
      }
    }
  }
  return 0;
}

// --layout-report COLSxROWS: the layout engine's decision, in text. The
// candidate table is scored by the SAME code compute_layout uses, so the
// "<-- chosen" row is exactly what the UI will render at this size.
int run_layout_report(const ui::Hardware& hw_in, int W, int H, const Options& opts) {
  // The SAME page the UI renders: the config (GPU overrides, order,
  // visibility) applied to the default page.
  const ui::Config cfg = ui::Config::load(
      opts.config_path.empty() ? ui::Config::default_path() : opts.config_path,
      /*no_save=*/true);
  ui::Hardware hw = hw_in;
  ui::apply_gpu_overrides(hw, cfg);
  const std::vector<ui::PanelSpec> page =
      ui::merge_page(ui::build_default_page(hw), cfg);
  ui::LayoutPrefs prefs;
  if (opts.columns > 0)
    prefs.columns = opts.columns;
  else if (cfg.columns > 0)
    prefs.columns = cfg.columns;
  const std::vector<ui::PanelSpec> vis = ui::visible_panels(page);

  const ui::Layout lo = ui::compute_layout(W, H, vis, prefs);
  const std::vector<ui::CandidateReport> cands =
      ui::score_layout_candidates(W, H, vis, prefs);

  const std::string pin = opts.columns > 0
                              ? "  (--columns " + std::to_string(opts.columns) + ")"
                              : "";
  const std::string hidden_note =
      page.size() != vis.size()
          ? "  (" + std::to_string(page.size() - vis.size()) + " hidden)"
          : "";
  std::printf("== GETH layout report: %dx%d, %zu panels%s%s ==\n", W, H,
              vis.size(), hidden_note.c_str(), pin.c_str());

  std::printf("\nfeasible candidates (ascending column count):\n");
  std::printf("  cols  mode      valid  degraded  min_graph_h  total_graph_area\n");
  for (const auto& cr : cands) {
    const char* mode = !cr.valid ? "invalid" : (cr.all_full ? "all-full" : "mixed");
    std::printf("  %4d  %-8s  %-5s  %8d  %11d  %16lld%s\n", cr.columns, mode,
                cr.valid ? "yes" : "no", cr.degraded, cr.min_graph_h,
                cr.total_graph_area,
                cr.columns == lo.columns ? "   <-- chosen" : "");
  }

  int n_full = 0, n_compact = 0, n_tight = 0;
  for (const auto& pl : lo.panels)
    if (pl.mode == ui::PanelMode::kFull) ++n_full;
    else if (pl.mode == ui::PanelMode::kCompact) ++n_compact;
    else ++n_tight;
  std::printf("\nchosen: %d column%s, %d full / %d compact / %d tight, "
              "scrolling=%s, page_height=%d content_height=%d\n",
              lo.columns, lo.columns == 1 ? "" : "s", n_full, n_compact, n_tight,
              lo.scrolling ? "yes" : "no", lo.page_height, lo.content_height);
  if (lo.scrolling)
    std::printf("  scroll window: %d of %d rows visible, offset 0..%d "
                "(the visible rows stretch to fill the viewport at every offset)\n",
                lo.rows(), lo.total_rows, lo.total_rows - lo.rows());

  std::printf("\npanels (reading order; rect x,y is content-relative, 0 = the "
              "top row under the header):\n");
  if (lo.scrolling)
    std::printf("  (scroll mode: y/h/mode below the first window are the offset-0 "
                "form; the renderer recomputes them per offset)\n");
  std::printf("  %-22s %4s %4s %4s %4s  %-8s  graph\n", "id", "x", "y", "w", "h",
              "mode");
  for (std::size_t i = 0; i < vis.size(); ++i) {
    const ui::LayoutRect& r = lo.panels[i].rect;
    const ui::PanelMode m = lo.panels[i].mode;
    const char* mname = m == ui::PanelMode::kFull     ? "full"
                      : m == ui::PanelMode::kCompact ? "compact"
                                                     : "tight";
    const int gw = ui::graph_w(vis[i], r.w, ui::uses_abbrev_labels(r.w - 2));
    const int gh = m == ui::PanelMode::kFull       ? ui::full_graph_h(vis[i], r.w, r.h)
                  : m == ui::PanelMode::kCompact   ? ui::compact_graph_h(r.h)
                                                   : ui::tight_graph_h(r.h);
    std::printf("  %-22s %4d %4d %4d %4d  %-8s  %dx%d\n", vis[i].id.c_str(), r.x, r.y,
                r.w, r.h, mname, gw, gh);
  }
  return 0;
}

// --perf: the Done-criterion performance evidence, reproducible. Two
// benchmarks: (1) relayout cost over 50 random terminal sizes, and (2) the
// layout-cache zero-allocation guarantee when the size is unchanged. The
// per-frame RENDER cost and allocation count are measured separately with
// `--render-frame COLSxROWS --stats` (FrameStats), which prints render
// time and allocations for one full frame.
int run_perf(const ui::Hardware& hw_in, const Options& opts) {
  // The SAME page the UI renders: the config (GPU overrides, order,
  // visibility) applied to the default page.
  const ui::Config cfg = ui::Config::load(
      opts.config_path.empty() ? ui::Config::default_path() : opts.config_path,
      /*no_save=*/true);
  ui::Hardware hw = hw_in;
  ui::apply_gpu_overrides(hw, cfg);
  const std::vector<ui::PanelSpec> page =
      ui::merge_page(ui::build_default_page(hw), cfg);
  const std::vector<ui::PanelSpec> vis = ui::visible_panels(page);
  ui::LayoutPrefs prefs;
  if (opts.columns > 0)
    prefs.columns = opts.columns;
  else if (cfg.columns > 0)
    prefs.columns = cfg.columns;
  std::printf("== GETH perf (%s), %zu panels%s ==\n",
              opts.demo ? "demo" : "real hardware", vis.size(),
              page.size() != vis.size() ? " visible" : "");

  // --- (1) relayout over 50 random sizes -------------------------------------
  // Fixed seed: reproducible across runs. The size range spans the test matrix
  // (60x20 .. 320x90) plus a little margin.
  std::mt19937 rng(20261015u);
  std::uniform_int_distribution<int> dw(60, 320), dh(20, 100);
  constexpr int kIters = 50;
  double mn = 1e30, mx = 0.0, sum = 0.0;
  for (int i = 0; i < kIters; ++i) {
    const int W = dw(rng), H = dh(rng);
    const auto t0 = std::chrono::steady_clock::now();
    ui::Layout lo = ui::compute_layout(W, H, vis, prefs);
    (void)lo;
    const auto t1 = std::chrono::steady_clock::now();
    const double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    mn = std::min(mn, us);
    mx = std::max(mx, us);
    sum += us;
  }
  std::printf("\nrelayout over %d random sizes (60x20 .. 320x100):\n", kIters);
  std::printf("  min %9.2f us   avg %9.2f us   max %9.2f us\n", mn, sum / kIters, mx);

  // --- (2) layout cache reuse: zero allocations when the size is unchanged ---
  ui::LayoutCache cache;
  cache.page = &page;
  cache.get(250, 70);  // one-time compute (allocates the layout vectors)
  set_alloc_counting(true);
  const long long abase = alloc_count();
  for (int i = 0; i < 1000; ++i) cache.get(250, 70);  // same size: reuse
  const long long reused = alloc_count() - abase;
  set_alloc_counting(false);
  std::printf("\nlayout cache reuse (1000 gets at 250x70, size unchanged):\n");
  std::printf("  allocations: %lld   (spec: 0)\n", reused);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  ui::frame_mark_process_start();  // the startup-time clock starts here
  // Before any tty work (probe, FTXUI's raw-mode setup): a backgrounded geth
  // (`geth &`) sits in a non-foreground process group of its controlling
  // terminal — the kernel would stop it on the tcsetattr (SIGTTOU), freezing
  // the UI on a blank alternate screen. See ignore_tty_stop_signals().
  util::ignore_tty_stop_signals();
  Options opts;
  if (parse_args(argc, argv, opts) != 0) return 2;

  if (opts.json && opts.probe) {
    std::fprintf(stderr, "geth: --probe and --json are mutually exclusive\n");
    return 2;
  }
  trace::start(opts.trace_series);  // --trace (empty = off)

  // SIGINT/SIGTERM can arrive during the (possibly long) probe phase; install
  // the stop flag before any hardware is touched. (In UI mode FTXUI adds its
  // own handlers — and the full terminal restore — once the loop starts.)
  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  if (opts.list_themes) {
    // Read-only: the config (for the active theme) and the themes directory.
    // No collector, no hardware.
    const ui::Config cfg = ui::Config::load(
        opts.config_path.empty() ? ui::Config::default_path() : opts.config_path,
        /*no_save=*/true);
    const std::string active = opts.theme.empty() ? cfg.theme : opts.theme;
    std::vector<std::string> warn;
    const auto themes = ui::all_themes(&warn);
    for (const auto& w : warn) std::fprintf(stderr, "geth: theme: %s\n", w.c_str());
    std::printf("themes (cycle with T in the UI):\n");
    for (const auto& [name, th] : themes) {
      (void)th;
      std::printf("  %s%s\n", name.c_str(), name == active ? "  <-- active" : "");
    }
    return 0;
  }

  if (opts.probe_terminal) {
    // Read-only: the config (for the graph_renderer intent) and the terminal
    // itself. No collector, no hardware.
    const ui::Config cfg = ui::Config::load(
        opts.config_path.empty() ? ui::Config::default_path() : opts.config_path,
        /*no_save=*/true);
    const std::string intent =
        opts.renderer.empty() ? cfg.graph_renderer : opts.renderer;
    std::string warn;
    const ui::ProbeResult probe = ui::probe_terminal();
    ui::resolve_protocol(intent, probe, &warn);  // only for the warning
    std::printf("%s", ui::probe_report_text(
                          probe, intent, !opts.renderer.empty()).c_str());
    if (!warn.empty())
      std::printf("  warning:            %s\n", warn.c_str());
    return 0;
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

  // The brief NVML enumeration session is a --probe-only feature: it must
  // never run at UI/dump startup (it can keep the GPU awake).
  if (registry) registry->set_probe_mode(opts.probe);

  if (opts.probe) return run_probe(collector, registry);

  collector.probe_all(/*warm_up=*/true);
  if (!collector.any_available()) {
    std::fprintf(stderr, "%s: no data source is available on this machine (see --probe)\n",
                 kFullName);
    return 1;
  }
  if (util::g_stop.load()) return 1;  // signalled during the probe
  // Gap detection (Part C) compares the time between samples against 5x this.
  collector.set_interval_ms(opts.interval_ms);

  // Discover the hardware once (from the warm-up sample) and hand it to the
  // UI. The probe_all(warm_up=true) above already took a reference sample, so
  // latest() is populated.
  const std::string cpu_name = read_cpu_model(opts.demo);
  const ui::Hardware hw = ui::hardware_from(collector.latest(), cpu_name);

  if (opts.layout_report) {
    int W = 0, H = 0;
    if (!parse_size(opts.layout_size, W, H)) {
      std::fprintf(stderr, "geth: --layout-report needs COLSxROWS, got '%s'\n",
                   opts.layout_size.c_str());
      return 2;
    }
    return run_layout_report(hw, W, H, opts);
  }

  if (opts.perf) return run_perf(hw, opts);

  if (opts.render_frame) {
    int W = 0, H = 0;
    if (!parse_size(opts.render_size, W, H)) {
      std::fprintf(stderr, "geth: --render-frame needs COLSxROWS, got '%s'\n",
                   opts.render_size.c_str());
      return 2;
    }
    ui::UiOptions u;
    u.interval_ms = opts.interval_ms;
    u.interval_explicit = opts.interval_explicit;
    u.color_mode = opts.color_mode;
    u.demo = opts.demo;
    u.ansi = opts.ansi;
    u.stats = opts.stats;
    u.columns = opts.columns;
    u.config_path = opts.config_path;
    u.no_save = opts.no_save;
    u.script = opts.script;
    u.theme = opts.theme;
    u.renderer = opts.renderer;
    return ui::run_render_frame(collector, hw, W, H, opts.warmup, u);
  }

  if (opts.render_png) {
    int W = 0, H = 0;
    if (!parse_size(opts.render_png_size, W, H)) {
      std::fprintf(stderr, "geth: --render-png needs COLSxROWS, got '%s'\n",
                   opts.render_png_size.c_str());
      return 2;
    }
    ui::UiOptions u;
    u.interval_ms = opts.interval_ms;
    u.interval_explicit = opts.interval_explicit;
    u.color_mode = opts.color_mode;
    u.demo = opts.demo;
    u.ansi = opts.ansi;
    u.stats = opts.stats;
    u.columns = opts.columns;
    u.config_path = opts.config_path;
    u.no_save = opts.no_save;
    u.script = opts.script;
    u.theme = opts.theme;
    u.renderer = opts.renderer;
    return ui::run_render_png(collector, hw, W, H, opts.warmup, u);
  }

  if (!opts.dump && !opts.json) {
    ui::UiOptions u;
    u.interval_ms = opts.interval_ms;
    u.interval_explicit = opts.interval_explicit;
    u.count = opts.count;
    u.color_mode = opts.color_mode;
    u.stats = opts.stats;
    u.columns = opts.columns;
    u.config_path = opts.config_path;
    u.no_save = opts.no_save;
    u.theme = opts.theme;
    u.renderer = opts.renderer;
    return ui::run_ui(collector, hw, u);
  }

  unsigned long long done = 0;
  while (!util::g_stop && (opts.count == 0 || done < opts.count)) {
    // A reference, not a copy: sample() fills the Collector's reused Snapshot
    // in place, so copying it here would allocate (strings/vectors) every tick.
    const Snapshot& snap = collector.sample();
    if (opts.json) {
      std::printf("%s\n", to_json(snap).c_str());
      std::fflush(stdout);
    } else {
      print_dump(snap);
    }
    ++done;
    if (util::g_stop || (opts.count != 0 && done >= opts.count)) break;
    if (opts.count == 0 || done < opts.count)
      sleep_interruptible(static_cast<double>(opts.interval_ms) / 1000.0);
  }
  return 0;
}
